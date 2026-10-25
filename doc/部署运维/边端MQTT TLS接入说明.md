# 边端 MQTT TLS 接入说明

本文说明边端当前 MQTT TLS 的实现、配置、证书登记、stunnel 兜底和排障方法。默认安装目录为 `/opt/modbus-gateway`。

## 1. 当前实现边界

边端 MQTT TLS 由内置 `BuiltinMqttDriverPublisher` 实现，不依赖外部 MQTT SDK。

相关代码：

- `include/edge_gateway/models.hpp`：`MqttTlsConfig` 和 `MqttConfig` 字段定义。
- `src/config_loader.cpp`：解析 app 配置中的 `mqtt.tls.*`。
- `src/builtin_mqtt_driver_publisher.cpp`：解析 broker、加载 OpenSSL、建立 TLS、校验证书、收发 MQTT 报文。
- `deploy/gateway-tls-enroll.sh`：设备证书的唯一入口——本机生成私钥与 CSR、校验并安装平台签发的证书、回退、查看（MQTT 客户端证书与维护通道证书同一条路径）。
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

## 3. 证书从哪里来（唯一路径）

设备证书只由平台信任服务签发，只走下面这一条路径；不再有入网令牌、设备直连平台、本地 bootstrap 根 CA、人工拷贝证书或 OTA 下发证书。

1. `production-init.sh` 写入 MQTT 配置。启用 MQTT TLS 时，`mqtt.tls` 固定指向 `config/runtime/tls/` 下的 `ca.crt`、`<machineCode>-client.pem`、`<machineCode>-client.key`；证书还没装好时不启动服务、不跑生产自检。
2. 工程客户端（已登录的工程师）在设备上执行 `gateway-tls-enroll.sh csr`：设备本机生成 ECDSA P-256 私钥（待用，`<key>.pending`）并输出 CSR，CN = 机器码。私钥不离开设备。
3. 客户端以工程师身份把 CSR 交给平台：`POST /api/v3/trust/certificates`（带 `Idempotency-Key`），平台按用途签发：`MQTT_CLIENT`（clientAuth）或 `MAINTENANCE_SERVER`（serverAuth），返回 `chainPem`（设备证书 + 设备 CA）与 `caPem`（平台根 CA 与服务 CA）。
4. 客户端把两个 PEM 写到设备临时文件，先 `install --dry-run`（全部校验、不写任何文件），确认后 `install`，然后重启服务并做健康确认；失败执行 `rollback` 回到安装前。
5. 维护通道证书走同一条路径（`--purpose maintenance`），客户端用平台下发的签名指纹清单核对设备出示的证书。

机器码只能使用 `A-Z a-z 0-9 . _ -`（证书 CN 与文件名，`production-init.sh`、本脚本和平台同一规则）。

## 4. gateway-tls-enroll.sh

```sh
gateway-tls-enroll.sh csr      --purpose mqtt|maintenance [--machine-code CODE] [--new-key]
gateway-tls-enroll.sh install  --purpose mqtt|maintenance --chain FILE --ca FILE [--machine-code CODE] [--dry-run]
gateway-tls-enroll.sh rollback --purpose mqtt|maintenance [--machine-code CODE]
gateway-tls-enroll.sh show     --purpose mqtt|maintenance [--machine-code CODE]
```

`--machine-code` 不填时读取 `config/runtime/device_identity.json`。

| 用途 | 私钥 | 证书 | CA |
| --- | --- | --- | --- |
| `mqtt` | `config/runtime/tls/<machineCode>-client.key`（600） | `config/runtime/tls/<machineCode>-client.pem`（证书 + 设备 CA） | `config/runtime/tls/ca.crt` |
| `maintenance` | `config/maintenance-v2/server.key`（600） | `config/maintenance-v2/server.crt`（证书 + 设备 CA） | — |

- `csr`：没有待用私钥时生成一把，已有则复用（同一 CSR 可重发）；`--new-key` 强制换新（轮换）。待用私钥在安装前不影响正在运行的服务。
- `install` 会拒绝（退出码 4）：证书不属于本机待用或当前私钥、CN 不是本机机器码、不是 P-256、链不到 `--ca` 给的平台根、过期、用途不对（MQTT 要 clientAuth，维护通道要 serverAuth）。链里缺设备 CA 或参数不全退出码 2。
- `install` 成功前把要被替换的文件留一份（`.previous-<purpose>/`），`rollback` 一步回到安装前；首次安装的回退会删掉新装的文件。
- 输出 `key=value`：`purpose`、`fingerprint`（SHA-256，大写冒号分隔，与平台、客户端的指纹清单一致）、`serial`、`notAfter`、`key=pending|current`、`replacesFingerprint`、`certFile`，以及 `dryRun=1` / `installed=1` / `rolledBack=1`。
- 脚本不改 app 配置、不重启服务、不联网：配置由 `production-init.sh` 写好，重启和健康确认由客户端做。

证书轮换：`csr --new-key` → 平台签发 → `install` → 重启 → 健康确认（失败 `rollback`）。平台对同一设备同一用途最多保留 4 张有效证书，超出时最旧的自动替代。

## 5. 出厂初始化中的 TLS

`production-init.sh` 只决定是否启用 MQTT TLS，不处理证书：

```sh
sh /opt/modbus-gateway/bin/production-init.sh \
  --machine-code GW0001 \
  --mqtt-broker ssl://broker.example.com:8883 \
  --mqtt-username mqtt-user \
  --mqtt-password mqtt-password \
  --mqtt-tls \
  --no-start --no-smoke
```

- broker 为 `ssl://`、`tls://`、`mqtts://` 或 `--mqtt-tls`（`INIT_MQTT_TLS_ENABLED=true`）时启用 TLS，`mqtt.tls` 的三个文件路径固定为上表 `mqtt` 一行，不接受自定义路径。
- 证书文件还不存在时，初始化完成配置后不启动服务、不跑自检，并提示下一步；装好证书后由客户端启动服务并跑 `production-smoke-test.sh`。
- 批量初始化（`batch-init-devices.sh`）同样只写配置，证书由客户端逐台安装。

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
openssl pkey -in /opt/modbus-gateway/config/runtime/tls/GW0001-client.key -noout -text | head -3
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

- 不要把私钥、MQTT 密码放入仓库。
- 出厂包只含目录和 stunnel 模板；设备证书只由 `gateway-tls-enroll.sh install` 落地。
- 客户端私钥权限建议 `600`。
- 生产必须开启服务端证书校验和 hostname 校验。
- 生产禁止使用 `insecureSkipVerify=true` 作为长期方案。
- broker 使用公网地址时，优先使用域名和公开可信或平台下发的 CA 链。
- OTA 包不能写入或清理设备私钥与证书（`config/runtime/tls/` 下除 `*.conf` 外的文件、`config/maintenance-v2/` 下除 `trusted-ticket-keys.json` 外的文件），`ota-apply.sh` 会拒绝；轮换走第 4 节。
