# 边端 MQTT TLS 接入说明

本文说明边端 MQTT 传输安全的实际实现、配置字段、发货模板与现场配置的关系、证书部署、现有明文现场的迁移步骤和排障方法。默认安装目录为 `/opt/modbus-gateway`。

## 0. 当前规则（本版本起）

- 默认只建立 TLS 连接，并且始终校验证书链和 broker 主机名/IP。
- 明文（`tcp://`、`mqtt://`、不写协议头）默认拒绝；只有显式配置实验开关 `allowInsecurePlaintext=true` 才允许，且持续告警。
- TLS 失败（证书错误、握手失败、缺库）只会按重连退避再次尝试 TLS，**不存在回落明文的代码路径**。
- `tls.insecureSkipVerify` 已取消：配置为 `true` 时拒绝连接，不再存在跳过校验的代码路径。
- broker 未配置、CA 文件未部署、系统信任库不存在时：配置照常加载，连接时拒绝并写明原因，且**不打开 TCP 连接**。不提供默认 broker 地址，不生成或伪造证书。

## 1. 客户端实际支持的能力

实现位置：`src/builtin_mqtt_driver_publisher.cpp` 中的 `evaluateMqttTransportPolicy`（按配置决定 TLS / 实验明文 / 拒绝）和 `connectMqttTransport`（建连）。所有 MQTT 客户端都经过同一个 `BuiltinMqttDriverPublisher` 建连，发布（`-tx`）和订阅（`-rx`）连接各自执行同一策略：

| 进程 | 配置来源 |
| --- | --- |
| `MqttDriver` | `mqtt-service.json:mqtt` |
| `EventEngine` | `mqtt-service.json:mqtt` |
| `SystemMonitor` | `monitor-service.json:mqtt` |
| `CameraService` | `camera-service.json:mqtt` |
| `MqttForwarder` 第三方转发 | `mqtt-service.json:mqttForward`（独立的 `tls` 和 `allowInsecurePlaintext`） |
| `MqttForwarder` 主平台 isolated full | 继承 `mqtt-service.json:mqtt` 的全部传输设置 |

| 项目 | 实际行为 |
| --- | --- |
| OpenSSL | 运行时成对加载 `libssl.so.1.1`+`libcrypto.so.1.1`，其次 `libssl.so.3`+`libcrypto.so.3`，再次 `libssl.so`+`libcrypto.so`。缺库或缺任何校验所需符号时拒绝连接。 |
| 协议版本 | 最低 TLS 1.2；上限和密码套件使用 OpenSSL 默认（1.1.1/3.x 可协商 TLS 1.3）。 |
| 证书链 | `SSL_VERIFY_PEER`。信任锚为 `tls.caFile`（PEM，可含多张 CA）；`caFile` 为空时使用系统信任库（`SSL_CERT_FILE`/`SSL_CERT_DIR` 或 OpenSSL 编译默认路径），系统信任库不存在则拒绝。 |
| 主机名 | broker 为域名：发送 SNI，并用 `X509_VERIFY_PARAM_set1_host` 校验证书 SAN（OpenSSL 规则；无 DNS SAN 时回退 CN）。 |
| IP 地址 | broker 为 IP：用 `X509_VERIFY_PARAM_set1_ip_asc` 校验证书 **IP SAN**，不发送 SNI；只有 CN=IP 的证书会被拒绝。 |
| 握手后确认 | 再次确认对端证书存在且校验结果为 OK。 |
| 双向 TLS | `certFile`/`keyFile`（PEM）成对配置；建连前检查文件可读、证书与私钥匹配。 |
| 错误信息 | 证书错误给出 OpenSSL 校验原因，例如 `unable to get local issuer certificate (X509 error 20)`、`hostname mismatch (62)`、`IP address mismatch (64)`、`certificate has expired (10)`；其他握手失败给出 OpenSSL 错误队列首条原因。 |
| 本地材料先检查 | CA、客户端证书、私钥缺失或不可用时在打开 socket 前拒绝。 |
| 日志 | 拒绝原因和 TLS 失败写入 stderr（journal），同一条消息 10 分钟最多记录一次；进程启动时若配置被拒绝也会记录一次。重连退避按 `reconnectBackoffMinMs`/`reconnectBackoffMaxMs`（默认 1 s 到 30 s）。 |
| 状态事件 | 订阅连接不可用时沿用 `mqtt-subscribe-unavailable`（`message` 带拒绝原因）；实验明文时 `MqttDriver` 每次启动发 `mqtt-insecure-plaintext` 状态事件。 |

不支持（固有限制）：跳过校验、证书固定（pinning）、CRL/OCSP 吊销检查、PKCS#12、带口令的私钥、WebSocket（`ws://`/`wss://`）、ALPN、自定义密码套件。吊销需要由 broker/平台侧通过缩短证书有效期、更换 CA 来处理。

上面是在本机 x86 OpenSSL 3.0 上用测试 broker 验证的行为（见 §9）；目标板 OpenSSL 1.1 的运行情况需现场验证。

## 2. 配置字段

字段位于各 app 配置的 `mqtt` 下；`mqttForward` 有同名的独立字段。

| 字段 | 含义 |
| --- | --- |
| `broker` | `ssl://host:8883`、`tls://`、`mqtts://`（不区分大小写）为 TLS；`tcp://`、`mqtt://` 或不写协议头为明文。端口缺省 TLS 8883、明文 1883。IPv6 写 `[addr]:port`。不支持 URL 内账号（`user:pw@host`，拒绝且不回显）。空字符串表示未部署，连接时拒绝。 |
| `tls.enabled` | `true` 时即使 broker 写 `tcp://` 也走 TLS；`false` 时由协议头决定。 |
| `tls.caFile` | broker 服务端证书的 CA（PEM）。为空时使用系统信任库。 |
| `tls.certFile` / `tls.keyFile` | 双向 TLS 的客户端证书和私钥（PEM，私钥不得加密），必须成对。 |
| `tls.insecureSkipVerify` | 已不支持。`true` 时拒绝连接；`false` 无作用，建议从配置中删除。 |
| `allowInsecurePlaintext` | 实验开关，默认 `false`。只有为 `true` 时才允许明文 broker；对 TLS broker 无作用。 |

`mqttForward` 规则相同，另有既有的加载期规则：启用 TLS 时 `mqttForward.tls.caFile` 必填。

出厂模板中的主 MQTT 配置：

```json
"mqtt": {
  "broker": "",
  "tls": {
    "enabled": true,
    "caFile": "/opt/modbus-gateway/config/runtime/tls/ca.crt",
    "certFile": "",
    "keyFile": ""
  },
  "allowInsecurePlaintext": false
}
```

`broker` 为空、`caFile` 指向尚未部署的路径，都是“需在部署时提供”的占位，不是可用地址或证书。

## 3. 模板、示例、打包脚本和运行配置的关系

| 路径 | 角色 | 怎样到设备上 | 本版本改动 |
| --- | --- | --- | --- |
| `config/factory/runtime/apps/{mqtt,monitor,camera}-service.json` | 出厂默认模板 | `deploy/build-factory-package.sh` 打进 `gateway-factory-defaults.tar.gz` 的 `config/factory`；`install-factory-config.sh` 复制到私有暂存区、写入取值后安装为设备 `config/runtime` | 改为 TLS 默认：`broker` 为空、`tls.enabled=true`、`caFile` 为部署路径、`allowInsecurePlaintext=false`、删除 `insecureSkipVerify` |
| `config/factory/runtime/tls/mqtt-service-stunnel.conf` | stunnel 模板 | 随包 | 未改 |
| `config/examples/*.json` | 示例 | 随包放在 `config/examples`，不会被自动加载 | `camera-service-public-example.json` 改为 `ssl://203.0.113.10:8883`（地址保持文档示例地址，8883 是 IANA 登记的 secure-mqtt 端口）；`mqtt-forward-disabled.json` 改为与出厂模板一致 |
| `config/runtime/apps/*.json` | 仓库内实验室运行快照（`tcp://192.168.22.102:1883`） | 不进出厂包，只在从仓库目录直接运行时读取 | 未改（运行配置不改）。新客户端会拒绝其明文连接 |
| `products/**/projects/**` | 项目快照 | 只包含设备点表，没有 MQTT app 配置 | 无关 |
| 设备 `/opt/modbus-gateway/config/runtime/apps/*.json` | 现场运行配置 | — | 不改；升级前按 §6 预检和迁移 |

初始化流程（`production-init.sh` 调用 `install-factory-config.sh`）：

1. 出厂初始化只在没有既有运行目录的设备上执行（`runtime-upgrade-guard.py factory` 要求 `config/runtime`、`data`、`bin` 为空），不会覆盖现场运行配置；现场设备的迁移按 §6 修改配置，不靠重新初始化。
2. 每个字段的取值优先级为：`INIT_*` 参数/环境变量，其次出厂模板。`broker` 不再兜底为 `tcp://127.0.0.1:1883`，未提供时保持为空，并打印 `WARNING: MQTT broker not configured`。
3. `insecureSkipVerify` 不再写入新配置；传 `INIT_MQTT_INSECURE_SKIP_VERIFY=true` 时，初始化在改动设备前拒绝执行（exit 2）。
4. 最终 broker 是明文且没有显式开关时，初始化在改动设备前退出（exit 2）。开关可用 `--mqtt-allow-insecure-plaintext` 或 `INIT_MQTT_ALLOW_INSECURE_PLAINTEXT=true` 开启，开启时打印风险警告并写入 `allowInsecurePlaintext=true`；交互模式会单独询问，默认否。
5. 出厂模板默认 `tls.enabled=true`。不带 `--no-mqtt-tls` 的初始化会进入证书部署流程（平台签发或 Win 签发包），缺少签发参数时按原有逻辑报错退出。
6. 平台签发由 `gateway-tls-enroll.sh` 完成，再由 `prepare-generic-runtime.py prepare-tls-apps` 写入 `tls` 路径，最后启动服务并执行 smoke。

OTA（`ota-apply.sh`）只按包内 manifest 替换文件：只含程序的包不改 `config/runtime`，旧现场的明文配置在 OTA 后保持不变，新程序随即拒绝连接，所以必须先按 §6 处理。manifest 也可以显式包含 `config/runtime/apps/*.json`、`config/runtime/tls/*`（`tls` 变更会重启 `gateway-services`），这是平台远程下发 TLS 配置和 CA 的途径；但 CA 或地址一旦写错，设备会经 MQTT 失联，只能走直连维护或 EasyTier 恢复。OTA 健康检查只看 systemd 单元是否 active，不会因 MQTT 被拒绝而自动回滚。`ota-apply.sh` 目前不阻止 OTA 配置包打开 `allowInsecurePlaintext`（`allowInsecureHttp` 有此限制），是否同样禁止待产品决定。

## 4. 新设备证书部署

以下三种方式任选其一。

A. 平台签发（推荐）：

```sh
sh /opt/modbus-gateway/bin/production-init.sh \
  --machine-code GW0001 \
  --mqtt-broker ssl://<broker 域名或 IP>:8883 \
  --mqtt-username <账号> --mqtt-password <口令> \
  --mqtt-tls \
  --tls-platform-url https://<平台地址> \
  --tls-token "$TLS_ENROLLMENT_TOKEN"
```

B. Win 签发包：同上，把平台参数换成 `--tls-ca-source`、`--tls-cert-source`、`--tls-key-source`。

C. 人工交付证书：

1. 用 `--no-mqtt-tls` 初始化，且不传 `--mqtt-broker`。此时 broker 为空，MQTT 保持拒绝。
2. 拷贝证书：
   ```sh
   install -m 0644 ca.crt /opt/modbus-gateway/config/runtime/tls/ca.crt
   # 仅双向 TLS：
   install -m 0644 GW0001-client.pem /opt/modbus-gateway/config/runtime/tls/GW0001-client.pem
   install -m 0600 GW0001-client.key /opt/modbus-gateway/config/runtime/tls/GW0001-client.key
   ```
3. 修改 `mqtt-service.json`、`monitor-service.json`、`camera-service.json` 的 `mqtt`：`broker=ssl://<broker>:8883`、`tls.enabled=true`、`tls.caFile`；双向 TLS 时再填 `certFile`/`keyFile`。
4. 执行 `systemctl restart gateway-services.service`，再执行 `production-smoke-test.sh`。

证书要求：

- `ca.crt` 必须是签发 broker 服务端证书的 CA。`--tls-generate-root-ca` 生成的本地 bootstrap CA 不是 broker 的信任锚，只有它时连接会因 `unable to get local issuer certificate` 被拒绝，这是预期行为。
- broker 服务端证书的 SAN 必须包含配置中的域名；用 IP 接入时必须包含该 IP 的 IP SAN。证书必须在有效期内；带 EKU 时必须包含 `serverAuth`。
- 客户端私钥权限为 `600`，私钥不得加密。不要把私钥、token 或 MQTT 口令放进仓库。

## 5. 实验开关 `allowInsecurePlaintext`

只允许在隔离的实验网络或台架上使用。开启后的风险：

- MQTT 账号和口令明文传输。
- 遥测、告警、OTA 和控制命令（含分合闸、功率、复位）明文传输，可被窃听。
- broker 未经认证，可被中间人冒充并下发命令。

开启后的表现：

- 每个进程启动时和每次建连都在 stderr 打印 `WARNING: INSECURE PLAINTEXT MQTT to tcp://…`。
- `MqttDriver` 每次启动发 `mqtt-insecure-plaintext` 状态事件。
- `production-smoke-test.sh` 判为 FAIL，不能作为生产验收通过。
- 对 TLS broker 无作用。

关闭方法：删除该字段或设为 `false`，然后重启服务。

stunnel 兜底方案：客户端到本机 stunnel 的一段是明文 `tcp://127.0.0.1:18883`，客户端无法确认隧道是否存在、是否校验证书，因此同样需要 `allowInsecurePlaintext=true`，并按实验模式告警和判定 smoke 失败。stunnel 本身也依赖 OpenSSL，推荐改用内置 TLS。`gateway-services.sh` 自动拉起 stunnel 的逻辑保持不变。

## 6. 现有明文现场的迁移（不改现场配置，由现场按步骤执行）

升级的影响：现场 app 配置仍是 `tcp://`、又没有 `allowInsecurePlaintext=true` 时，升级到本版本后所有 MQTT 客户端都会拒绝连接：

- 平台侧设备离线，经 MQTT 的 OTA、远程命令和配置下发都不可用。
- 本地采集、控制逻辑、本地画面、直连维护（9443）和 EasyTier 不受影响。
- 每次连接被拒绝都会写入 journal（每 10 分钟最多一条）：`mqtt connection refused clientId=…: plaintext broker tcp://… refused …`。

步骤：

1. **升级前只读预检**，使用新包中的脚本：
   ```sh
   GATEWAY_HOME=/opt/modbus-gateway sh <新包目录>/deploy/production-smoke-test.sh --mqtt-transport
   ```
   退出码 0 表示新版本可以建立连接；退出码 1 时逐条查看 `[FAIL]`。
2. 预检失败时二选一：
   - **方案 A（推荐）迁移到 TLS**。旧版本程序同样支持 `ssl://` 和证书校验，可以在升级前完成并验证：
     1. broker 侧开启 8883 TLS 监听，部署服务端证书（SAN 与边端配置的域名/IP 一致）。
     2. 边端部署 `ca.crt`（平台签发或人工拷贝）；双向 TLS 时还要部署客户端证书和私钥。
     3. 修改现场 `mqtt-service.json`、`monitor-service.json`、`camera-service.json`（以及已启用的 `mqttForward`）：`broker=ssl://<同一 broker>:8883`、`tls.enabled=true`、`tls.caFile`、按需 `certFile`/`keyFile`，删除 `insecureSkipVerify`。
     4. 重启服务，确认旧版本已通过 TLS 连上，再执行升级。
   - **方案 B（临时）显式实验开关**：在现场 `mqtt` 下加 `"allowInsecurePlaintext": true`。旧版本会忽略这个字段，可以在升级前加。升级后继续明文连接，持续告警且 smoke 判 FAIL，必须排期迁移到方案 A。注意：`mqttForward` 的同名字段只能在升级后再加，旧版本会以 `mqttForward has unsupported key` 拒绝加载整个配置。
3. 现场若使用 `insecureSkipVerify=true`：先部署正确的 CA 并把它改为 `false`，在旧版本上确认能连接后再升级。
4. 若现场使用 IP 接入 TLS broker，确认服务端证书带 IP SAN：
   ```sh
   openssl s_client -connect <ip>:8883 -CAfile ca.crt -verify_ip <ip> -verify_return_error
   ```
5. 升级后验证：journal 中没有 `mqtt connection refused` 或 `mqtt tls …`；平台收到 `started` 状态；`production-smoke-test.sh` 结果为 `fail=0`。
6. 升级后才发现断连：通过直连维护或 EasyTier 登录，按第 2 步修改配置并重启 `gateway-services.service`，或回滚程序。


## 7. 验证命令

```sh
ldconfig -p | grep -E 'libssl|libcrypto'
GATEWAY_HOME=/opt/modbus-gateway sh /opt/modbus-gateway/bin/production-smoke-test.sh --mqtt-transport
openssl x509 -in /opt/modbus-gateway/config/runtime/tls/ca.crt -noout -subject -issuer -dates
openssl s_client -connect <broker>:8883 -servername <broker> \
  -CAfile /opt/modbus-gateway/config/runtime/tls/ca.crt -verify_hostname <broker> -verify_return_error
journalctl -u mqtt-driver@mqtt-service.service -n 200 --no-pager | grep -E 'mqtt (connection refused|tls)|INSECURE'
```

双向 TLS 时，在 `s_client` 上加 `-cert <client.pem> -key <client.key>`。

## 8. 常见错误

| 日志 | 原因 | 处理 |
| --- | --- | --- |
| `mqtt broker is not configured` | `broker` 为空（出厂模板或未传 `--mqtt-broker`） | 填写 `ssl://<broker>:8883` |
| `plaintext broker tcp://… refused` | 明文 broker，未开实验开关 | 按 §6 迁移到 TLS |
| `tls.insecureSkipVerify=true is not supported` | 配置仍要求跳过校验 | 部署 CA，删除该字段 |
| `mqtt tls caFile is missing or unreadable: …` | CA 未部署或权限不对 | 部署 CA 文件 |
| `mqtt tls caFile is not a usable PEM CA certificate` | 文件不是 PEM 证书 | 换成正确的 PEM CA |
| `tls.caFile is not configured and no system trust store was found` | `caFile` 为空且系统无 CA 库 | 配置 `tls.caFile` |
| `certificate verification failed …: unable to get local issuer certificate` | CA 与 broker 证书不匹配 | 换成签发 broker 证书的 CA |
| `…: hostname mismatch` / `IP address mismatch` | 证书 SAN 不含配置的域名/IP | 修正 broker 地址或重签证书 |
| `…: certificate has expired` / `certificate is not yet valid` | 证书过期，或设备时间错误 | 续签证书，或校正设备时间（校时不在本文范围） |
| `mqtt tls handshake failed …` | 端口不是 TLS、TLS 版本低于 1.2、broker 要求客户端证书等 | 用 `openssl s_client` 排查 |
| `mqtt tls requested but libssl is not available` | 系统没有 OpenSSL 1.1/3.x | 安装 libssl |
| `mqtt tls certFile/keyFile is missing or unreadable` / `client certificate and key mismatch` | 双向 TLS 材料有误 | 重新签发或修正路径 |
| `WARNING: INSECURE PLAINTEXT MQTT` | 实验开关已开启 | 仅限实验；按 §6 迁移 |

## 9. 验证范围

| 项目 | 状态 |
| --- | --- |
| `tools/mqtt_tls_transport_test.cpp`（ctest `mqtt_tls_transport`） | 本机模拟通过。在临时目录用 openssl 现场生成 CA、服务端和客户端证书，启动本地 TLS 测试 broker（`tools/mqtt_tls_test_broker.py`），覆盖：正常 TLS（域名+SNI、IP SAN、系统信任库）、错误 CA、主机名不符、IP 不符、证书过期、缺 CA/坏 CA/无信任库、insecureSkipVerify、双向 TLS、明文默认拒绝（不建 socket）、TLS 失败不回落明文、实验开关可连且告警。 |
| `tools/mqtt_transport_deploy_policy_test.sh`（ctest `mqtt_transport_deploy_policy`） | 本机模拟通过。覆盖初始化脚本的拒绝规则和 smoke 预检。 |
| 目标板 OpenSSL 1.1、真实 broker、真实平台签发证书 | 现场待验证。 |
