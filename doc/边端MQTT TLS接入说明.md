# 边端 MQTT TLS 接入说明

本文说明边端当前 MQTT TLS 的实现、配置、证书登记、stunnel 兜底和排障方法。默认安装目录为 `/opt/modbus-gateway`。

## 1. 当前实现边界

边端 MQTT TLS 由内置 `BuiltinMqttDriverPublisher` 实现，不依赖外部 MQTT SDK。

相关代码：

- `include/edge_gateway/models.hpp`：`MqttTlsConfig` 和 `MqttConfig` 字段定义。
- `src/config_loader.cpp`：解析 app 配置中的 `mqtt.tls.*`。
- `src/builtin_mqtt_driver_publisher.cpp`：解析 broker、加载 OpenSSL、建立 TLS、校验证书、收发 MQTT 报文。
- `deploy/gateway-tls-enroll.sh`：生成客户端私钥/CSR，并向平台申请签发证书。
- `deploy/gateway-services.sh`：按 app broker 自动选择是否启动 `mqtt-tls-tunnel@*.service`。
- `deploy/production-smoke-test.sh`：生产验收时检查 MQTT TLS、证书文件和 stunnel 配置。

支持能力：

- broker 协议头 `ssl://`、`tls://`、`mqtts://` 自动启用 TLS。
- `mqtt.tls.enabled=true` 可在 broker 协议头不是 TLS 时强制启用 TLS。
- 服务端证书校验、CA 文件校验、系统 CA 路径兜底。
- SNI 和 hostname 校验。
- 双向 TLS：`certFile` 和 `keyFile` 成对配置。
- MQTT3 / MQTT5、用户名密码、QoS、订阅和发布在 TLS 通道上复用同一套 MQTT 编码逻辑。
- OpenSSL 不可用时，可用本机 stunnel 作为 TLS 隧道兜底。

不支持或需要注意：

- 生产禁止 `insecureSkipVerify=true`。该选项只允许临时联调。
- 如果 broker 使用 IP 地址，服务端证书必须包含对应 IP SAN；否则 hostname 校验会失败。生产推荐使用 DNS 域名。
- `certFile` 和 `keyFile` 必须同时为空或同时填写，不能只填其中一个。
- 内置 TLS 运行时需要系统存在 `libssl` 和 `libcrypto`，当前会动态尝试加载 `libssl.so.1.1`、`libssl.so.3`、`libssl.so` 以及对应 `libcrypto`。

## 2. 配置字段

TLS 配置位于每个 app 配置的 `mqtt.tls` 下。常见文件：

- `/opt/modbus-gateway/config/runtime/apps/mqtt-service.json`
- `/opt/modbus-gateway/config/runtime/apps/monitor-service.json`
- `/opt/modbus-gateway/config/runtime/apps/camera-service.json`

字段说明：

| 字段 | 含义 |
| --- | --- |
| `mqtt.broker` | MQTT broker 地址，TLS 推荐 `ssl://broker.example.com:8883` |
| `mqtt.tls.enabled` | 是否强制启用 TLS；`ssl://`、`tls://`、`mqtts://` 会自动启用 |
| `mqtt.tls.caFile` | 服务端证书 CA 文件路径；为空时使用系统默认 CA 路径 |
| `mqtt.tls.certFile` | 客户端证书路径，只有 broker 要求双向 TLS 时填写 |
| `mqtt.tls.keyFile` | 客户端私钥路径，必须和 `certFile` 成对填写 |
| `mqtt.tls.insecureSkipVerify` | 跳过服务端证书和 hostname 校验，生产禁止 |

单向 TLS 示例：

```json
{
  "mqtt": {
    "enabled": true,
    "protocolVersion": "mqtt5",
    "broker": "ssl://broker.example.com:8883",
    "clientId": "GW0001",
    "username": "mqtt-user",
    "password": "mqtt-password",
    "tls": {
      "enabled": true,
      "caFile": "/opt/modbus-gateway/config/runtime/tls/ca.crt",
      "certFile": "",
      "keyFile": "",
      "insecureSkipVerify": false
    }
  }
}
```

双向 TLS 示例：

```json
{
  "mqtt": {
    "broker": "ssl://broker.example.com:8883",
    "tls": {
      "enabled": true,
      "caFile": "/opt/modbus-gateway/config/runtime/tls/ca.crt",
      "certFile": "/opt/modbus-gateway/config/runtime/tls/GW0001-client.pem",
      "keyFile": "/opt/modbus-gateway/config/runtime/tls/GW0001-client.key",
      "insecureSkipVerify": false
    }
  }
}
```

## 3. 内置 TLS 接入流程

推荐生产流程不再手工预置客户端证书，而是在初始化或维护时走 enrollment：

1. 平台启用 TLS enrollment。平台如果没有显式配置 CA 证书和私钥，会在平台配置根目录自动生成并持久化 `runtime/tls/enrollment-ca.crt` 和 `runtime/tls/enrollment-ca.key`。
2. 边端初始化时明确输入 `machineCode`。该值会写入 `device_identity.json`，并作为 MQTT `clientId` 和后续 topic 定向依据。
3. 边端执行 `production-init.sh --tls-generate-root-ca --tls-platform-url ... --tls-token ...`，由边端本机生成私钥/CSR，平台签发客户端证书，边端落盘 `ca.crt`、`<machineCode>-client.pem`、`<machineCode>-client.key`。
4. 初始化脚本更新 app 中的 `mqtt.tls` 路径并重启服务。

目录和权限仍应满足：

```sh
mkdir -p /opt/modbus-gateway/config/runtime/tls
chmod 755 /opt/modbus-gateway/config/runtime/tls
```

如果现场采用人工证书交付，不走平台签发，可以手工放入 CA、客户端证书和私钥：

```sh
cp ca.crt /opt/modbus-gateway/config/runtime/tls/ca.crt
cp GW0001-client.pem /opt/modbus-gateway/config/runtime/tls/GW0001-client.pem
cp GW0001-client.key /opt/modbus-gateway/config/runtime/tls/GW0001-client.key
chmod 644 /opt/modbus-gateway/config/runtime/tls/ca.crt
chmod 644 /opt/modbus-gateway/config/runtime/tls/GW0001-client.pem
chmod 600 /opt/modbus-gateway/config/runtime/tls/GW0001-client.key
```

然后修改需要连接 broker 的 app 配置。

至少确认：

- `mqtt-service.json`：第三方转发、全量上传、实时请求、控制、OTA、事件补发。
- `monitor-service.json`：系统监测、诊断、配置拉取。
- `camera-service.json`：摄像头服务启用并通过 MQTT 上报状态时才需要。

最后重启统一服务入口：

```sh
systemctl restart gateway-services.service
```

查看目标服务日志：

```sh
journalctl -u gateway-services.service -n 100 --no-pager
journalctl -u mqtt-driver@mqtt-service.service -n 200 --no-pager
journalctl -u system-monitor@monitor-service.service -n 200 --no-pager
```

## 4. 证书登记和签发

边端提供 `gateway-tls-enroll.sh` 用于生成私钥、CSR，并可调用平台签发接口。

只生成私钥和 CSR：

```sh
GATEWAY_HOME=/opt/modbus-gateway \
sh /opt/modbus-gateway/bin/gateway-tls-enroll.sh \
  --machine-code GW0001 \
  --generate-root-ca
```

输出文件默认位于：

```text
/opt/modbus-gateway/config/runtime/tls/ca.crt
/opt/modbus-gateway/config/runtime/tls/ca.key
/opt/modbus-gateway/config/runtime/tls/GW0001-client.key
/opt/modbus-gateway/config/runtime/tls/GW0001-client.csr
```

向平台申请签发并更新本地 app 配置：

```sh
GATEWAY_HOME=/opt/modbus-gateway \
sh /opt/modbus-gateway/bin/gateway-tls-enroll.sh \
  --machine-code GW0001 \
  --platform-url https://platform.example.com \
  --token "$TLS_ENROLLMENT_TOKEN" \
  --generate-root-ca \
  --validity-days 365 \
  --update-local-app \
  --restart
```

签发流程：

- 生成或复用 `/opt/modbus-gateway/config/runtime/tls/<machineCode>-client.key`。
- 生成 CSR，subject 为 `/CN=<machineCode>/O=KYXN/OU=edge-gateway`。
- 如果传入 `--generate-root-ca`，先生成本地 bootstrap 根 CA，便于离线检查证书目录；平台签发成功后，最终 `ca.crt` 以平台返回的 CA 为准。
- 请求平台接口 `/api/config/tls/enrollment/sign`。
- 请求头使用 `X-Edge-Tls-Enrollment-Token`。
- 平台返回 `certificatePem` 和 `caPem` 后，边端写入 `<machineCode>-client.pem` 和 `ca.crt`。
- `--update-local-app` 会把 app 配置中的 `mqtt.tls` 改为启用，并写入 CA、证书、私钥路径。

注意：

- 私钥文件权限应保持 `600`。
- 不要把私钥、真实 token、MQTT 密码提交进仓库。
- `--force-key` 会重新生成私钥，旧证书会失配，需重新签发。

## 5. 出厂初始化中的 TLS

`deploy/production-init.sh` 支持通过命令行或环境变量写入 MQTT TLS 参数。

常用参数：

```sh
sh /opt/modbus-gateway/bin/production-init.sh \
  --machine-code GW0001 \
  --mqtt-broker ssl://broker.example.com:8883 \
  --mqtt-username mqtt-user \
  --mqtt-password mqtt-password \
  --mqtt-tls \
  --tls-ca-file /opt/modbus-gateway/config/runtime/tls/ca.crt \
  --tls-cert-file /opt/modbus-gateway/config/runtime/tls/GW0001-client.pem \
  --tls-key-file /opt/modbus-gateway/config/runtime/tls/GW0001-client.key \
  --tls-generate-root-ca \
  --tls-platform-url https://platform.example.com \
  --tls-token "$TLS_ENROLLMENT_TOKEN" \
  --tls-validity-days 365
```

对应环境变量：

```sh
INIT_MACHINE_CODE=GW0001
INIT_MQTT_BROKER=ssl://broker.example.com:8883
INIT_MQTT_USERNAME=mqtt-user
INIT_MQTT_PASSWORD=mqtt-password
INIT_MQTT_TLS_ENABLED=true
INIT_MQTT_CA_FILE=/opt/modbus-gateway/config/runtime/tls/ca.crt
INIT_MQTT_CERT_FILE=/opt/modbus-gateway/config/runtime/tls/GW0001-client.pem
INIT_MQTT_KEY_FILE=/opt/modbus-gateway/config/runtime/tls/GW0001-client.key
INIT_MQTT_INSECURE_SKIP_VERIFY=false
INIT_TLS_GENERATE_ROOT_CA=1
INIT_TLS_PLATFORM_URL=https://platform.example.com
INIT_TLS_ENROLLMENT_TOKEN=...
INIT_TLS_VALIDITY_DAYS=365
```

当前 `production-init.sh` 在检测到 TLS broker、`INIT_MQTT_TLS_ENABLED=true` 或 TLS 签发参数时，会走证书部署流程。量产初始化启用 MQTT TLS 时，必须提供 `INIT_TLS_PLATFORM_URL` 和 `INIT_TLS_ENROLLMENT_TOKEN`，由平台签发客户端证书。只传 `INIT_TLS_GENERATE_ROOT_CA=1` 时只会准备本地 bootstrap CA、客户端私钥和 CSR，不会让 MQTT TLS 半成品配置生效。

如果现场采用人工拷贝证书，不走平台签发，应先完成非 TLS 或不启动 MQTT TLS 的初始化，再手工放置证书、修改 runtime app 配置并重启 `gateway-services.service`。

## 6. stunnel 兜底方案

如果目标系统无法提供可用的 OpenSSL 运行库，或内置 TLS 因系统库限制无法进行 hostname 校验，可使用本机 stunnel。

安装依赖：

```sh
apt-get update
apt-get install -y stunnel4
```

app 配置改为连接本机明文端口：

```json
{
  "mqtt": {
    "broker": "tcp://127.0.0.1:18883",
    "tls": {
      "enabled": false,
      "caFile": "",
      "certFile": "",
      "keyFile": "",
      "insecureSkipVerify": false
    }
  }
}
```

stunnel 配置示例：

```text
foreground = yes
pid =
client = yes

[mqtts]
accept = 127.0.0.1:18883
connect = broker.example.com:8883
CAfile = /opt/modbus-gateway/config/runtime/tls/ca.crt
verifyChain = yes
checkHost = broker.example.com
```

保存路径示例：

```text
/opt/modbus-gateway/config/runtime/tls/mqtt-service-stunnel.conf
```

服务启动逻辑：

- `gateway-services.sh` 会扫描 app 配置。
- 当 app 的 `mqtt.broker` 指向 `127.0.0.1` 或 `localhost` 的非默认本地端口时，会在 `config/runtime/tls` 下查找匹配的 `*-stunnel.conf`。
- 匹配规则优先使用 `<appName>-stunnel.conf`，例如 `mqtt-service-stunnel.conf`。
- `accept` 的端口必须和 app broker 端口一致。
- 匹配成功后自动启动 `mqtt-tls-tunnel@<confName>.service`。
- 不需要单独 enable 某个 `mqtt-tls-tunnel@...` 实例。

验证期望：

```sh
/opt/modbus-gateway/bin/gateway-services.sh list
systemctl status mqtt-tls-tunnel@mqtt-service-stunnel.service --no-pager
journalctl -u mqtt-tls-tunnel@mqtt-service-stunnel.service -n 100 --no-pager
```

## 7. 验证命令

检查 OpenSSL 运行库：

```sh
ldconfig -p | grep -E 'libssl|libcrypto'
```

检查 app TLS 配置：

```sh
grep -nE '"broker"|"tls"|"caFile"|"certFile"|"keyFile"|"insecureSkipVerify"' \
  /opt/modbus-gateway/config/runtime/apps/mqtt-service.json \
  /opt/modbus-gateway/config/runtime/apps/monitor-service.json
```

检查证书文件：

```sh
ls -l /opt/modbus-gateway/config/runtime/tls
openssl x509 -in /opt/modbus-gateway/config/runtime/tls/ca.crt -noout -subject -issuer -dates
openssl x509 -in /opt/modbus-gateway/config/runtime/tls/GW0001-client.pem -noout -subject -issuer -dates
openssl rsa -in /opt/modbus-gateway/config/runtime/tls/GW0001-client.key -check -noout
```

手动测试 broker 握手：

```sh
openssl s_client \
  -connect broker.example.com:8883 \
  -servername broker.example.com \
  -CAfile /opt/modbus-gateway/config/runtime/tls/ca.crt \
  -verify_return_error
```

双向 TLS 握手：

```sh
openssl s_client \
  -connect broker.example.com:8883 \
  -servername broker.example.com \
  -CAfile /opt/modbus-gateway/config/runtime/tls/ca.crt \
  -cert /opt/modbus-gateway/config/runtime/tls/GW0001-client.pem \
  -key /opt/modbus-gateway/config/runtime/tls/GW0001-client.key \
  -verify_return_error
```

生产冒烟测试：

```sh
/opt/modbus-gateway/bin/gateway-run.sh smoke
```

验收标准：

- `fail=0`。
- 生产环境 `tls.insecureSkipVerify` 必须为 `false`。
- TLS broker 对应的 CA 文件存在，或明确使用系统 CA。
- `certFile` 和 `keyFile` 要么都为空，要么都存在。
- 如果 app broker 指向本机 stunnel 端口，必须存在匹配的 stunnel 配置，且 `connect`、`checkHost`、`CAfile` 不能保留模板值。

## 8. 常见错误和处理

| 日志或现象 | 可能原因 | 处理 |
| --- | --- | --- |
| `mqtt tls requested but libssl is not available` | 系统没有 OpenSSL 运行库 | 安装 `libssl`，或改用 stunnel |
| `mqtt tls requested but required OpenSSL symbols are missing` | OpenSSL 版本或符号不满足当前实现 | 换 `libssl.so.1.1` / `libssl.so.3`，或改用 stunnel |
| `mqtt tls CA file load failed` | `caFile` 路径错误、文件不存在或不是 PEM CA | 修正路径和文件内容 |
| `mqtt tls certFile and keyFile must be configured together` | 只配置了证书或只配置了私钥 | 两个字段同时填写或同时清空 |
| `mqtt tls client certificate load failed` | 客户端证书路径错误或格式错误 | 检查 PEM 文件 |
| `mqtt tls client private key load failed` | 私钥路径错误、权限不足或格式错误 | 检查路径、权限和 PEM 格式 |
| `mqtt tls client certificate and key mismatch` | 证书和私钥不是一对 | 重新签发证书或更换对应私钥 |
| `mqtt tls hostname verification setup failed` | OpenSSL 缺少 hostname 校验符号 | 换可用 OpenSSL，或用 stunnel |
| `mqtt tls handshake failed` | 端口不是 TLS、协议不匹配、broker 拒绝证书或网络中断 | 用 `openssl s_client` 先验证链路 |
| `mqtt tls certificate verification failed` | CA 不匹配、证书过期、hostname 不匹配 | 换正确 CA，检查证书有效期和 SAN |
| MQTT 连接反复重试但 TLS 无明显错误 | 用户名密码、clientId 或 broker ACL 不通过 | 检查 broker 日志和 MQTT 认证配置 |

## 9. 生产安全要求

- 不要把真实 CA 私钥、客户端私钥、MQTT 密码、TLS enrollment token 放入仓库。
- 出厂包可包含目录和模板，真实证书应在设备初始化或平台签发时落地到 `config/runtime/tls`。
- 客户端私钥权限建议 `600`。
- 生产必须开启服务端证书校验和 hostname 校验。
- 生产禁止使用 `insecureSkipVerify=true` 作为长期方案。
- broker 使用公网地址时，优先使用域名和公开可信或平台下发的 CA 链。
- 证书轮换通过 OTA 下发 `config/runtime/tls/*` 时，manifest 应触发 `gateway-services.service` 重启，使 MQTT 服务重新建连。
