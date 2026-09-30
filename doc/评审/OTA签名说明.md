# OTA 签名说明（edge-ota-v1）

自本版本起，边端默认 `ota.requireSignature=true`：MQTT OTA 请求必须携带 `signature` 字段，且签名必须能被设备上的公钥
`ota.signaturePublicKeyPath`（默认 `/etc/modbus-gateway/ota.pub`）验证，否则任务在下载前被拒绝（`ota signature is required` /
`ota signature verification failed`）。签名任务同时强制要求 `sha256`，下载后仍按 `sha256` 校验包体。

## 1. 算法

- `openssl dgst -sha256` 签名/验签，支持 ECDSA P-256（推荐）或 RSA ≥ 3072 位公钥（PEM）。
- 设备端以 argv 方式（无 shell）调用：
  `openssl dgst -sha256 -verify <公钥> -signature <签名二进制> <描述串文件>`
- `signature` 字段内容 = 签名二进制的标准 Base64（无换行，允许 `=` 填充）。

## 2. 规范描述串（canonical descriptor）

按以下固定顺序拼接，每行以单个 `\n`（LF）结尾，最后一行后也有 `\n`，无多余空格、无 BOM，UTF-8：

```
edge-ota-v1
jobId=<jobId>
machineCode=<目标网关 machineCode>
packageType=<packageType，未下发则为空>
version=<version，未下发则为空>
sha256=<sha256，小写十六进制>
size=<size，十进制；未下发为 0>
```

说明：

- 取值与 MQTT 请求中的同名字段完全一致（`sha256` 统一转小写）；`artifactUrl` 不参与签名，便于使用预签名 URL，包体完整性由已签名的 `sha256` 保证。
- `jobId` 在设备上一次性使用：进入 apply 阶段后（或已成功完成）再次下发同一 `jobId` 会被拒绝（`ota jobId already processed`）。重试请换新的 `jobId` 并重新签名。
- `machineCode` 参与签名，签名只对目标网关有效。

## 3. 生成密钥（离线签名机执行，私钥不得进入设备或代码仓库）

```sh
openssl ecparam -name prime256v1 -genkey -noout -out ota-signing.key
openssl ec -in ota-signing.key -pubout -out ota.pub
# 设备侧安装公钥（不可放在 OTA 可写的 /opt/modbus-gateway 下）
install -D -m 0644 ota.pub /etc/modbus-gateway/ota.pub
```

出厂流程自动下发公钥：

```sh
# 打包时带上公钥（或 OTA_SIGNING_PUBKEY=/path/ota.pub），包内路径 ota/ota.pub
deploy/build-factory-package.sh --ota-pubkey /secure/ota.pub
# install-factory-config.sh / production-init.sh 发现 ota/ota.pub（或 OTA_SIGNING_PUBKEY /
# production-init.sh --ota-pubkey FILE）时安装到 /etc/modbus-gateway/ota.pub（0644 root）；
# 缺失时打印醒目 WARNING：此时设备会拒绝所有 OTA。私钥会被拒绝打包/安装。
```

## 4. 签名示例

```sh
printf 'edge-ota-v1\njobId=%s\nmachineCode=%s\npackageType=%s\nversion=%s\nsha256=%s\nsize=%s\n' \
  "JOB_20260929_001" "GW_PROD_001" "config" "config-20260929.1" \
  "$(sha256sum gateway-config.tar.gz | cut -c1-64)" "$(stat -c %s gateway-config.tar.gz)" > descriptor.txt
openssl dgst -sha256 -sign ota-signing.key -out descriptor.sig descriptor.txt
SIGNATURE=$(openssl base64 -A -in descriptor.sig)
# 自检
openssl dgst -sha256 -verify ota.pub -signature descriptor.sig descriptor.txt
```

MQTT 请求示例（`size`/`sha256`/`version`/`packageType` 必须与描述串一致）：

```json
{"jobId":"JOB_20260929_001","machineCode":"GW_PROD_001","packageType":"config",
 "version":"config-20260929.1","artifactUrl":"https://ota.example.com/gateway-config.tar.gz",
 "sha256":"<64位小写hex>","size":123456,"signature":"<SIGNATURE>"}
```

## 5. 相关配置（`apps/*.json` 的 `ota` 节）

| 键 | 默认值 | 说明 |
|---|---|---|
| `requireSignature` | `true` | 关闭仅用于实验室，生产禁止 |
| `signaturePublicKeyPath` | `/etc/modbus-gateway/ota.pub` | 验签公钥（PEM） |
| `allowInsecureHttp` | `false` | 为 `false` 时拒绝 `http://` 下载源；curl 同时限制 `--proto =https --proto-redir =https` |

本地回退（仅限尚未下发公钥的存量设备/实验室，需现场或离线维护通道操作，OTA 包无法做到）：
在设备 `apps/mqtt-service.json` 的 `ota` 节写入 `"requireSignature": false`（http 源另需 `"allowInsecureHttp": true`），
然后重启 `mqtt-driver@mqtt-service.service`。此时仍强制 `checksumRequired` 的 sha256 校验，但不再有来源认证，
公钥下发后应尽快改回 `true`（可通过已签名的 OTA 配置包恢复为 `true`）。

## 6. 相关运维行为（ota-apply.sh / ota-rollback.sh）

- `jobId` 一次性：`ota/staging/applied_jobs.txt` 记录进入 apply 的任务，已存在的 `staging/<jobId>`、`backup/<jobId>` 或成功记录都会拒绝重放。
- 断电保护：修改运行时前写入并 fsync `ota/staging/ota-in-progress`，完成后删除；开机由 `gateway-ota-recover.service`
  （`Before=gateway-services.service`）调用 `ota-rollback.sh --recover-interrupted` 回滚未完成任务（开机恢复不重启服务）。
  标记存在时新的 OTA 会被拒绝，需先完成恢复。
- 重启后健康检查：重启序列通过 `systemd-run` 独立运行，等待 `OTA_HEALTH_VERIFY_SEC`（默认 30 秒）后检查被重启单元（含
  `gateway-services.sh list` 中的单元）是否 active，不健康时自动执行 `ota-rollback.sh`，结果写入 `ota/staging/health_<jobId>.txt`。
  `OTA_AUTO_ROLLBACK=0` 可关闭（需通过 mqtt-driver 单元 drop-in 设置环境变量）。注意：边端在健康检查前已上报 `completed`。
- 容量：`staging`/`backup` 仅保留最近 `OTA_RETENTION_JOBS`（默认 3）个任务目录；SCADA 仅保留当前、上一版及最近 `SCADA_KEEP_RELEASES`（默认 3）个发布。
- OTA 包只能收紧、不能放松 OTA 安全配置：可以把 `requireSignature`/`checksumRequired` 设为 `true`、把 `allowInsecureHttp` 设为 `false`；
  不能关闭签名/校验、不能开启 http、不能修改 `signaturePublicKeyPath`、`applyScript`、`rollbackScript`（未写时按默认值比较）。
- 仅在修改运行时之前才记录 `jobId`；在此之前失败（包校验失败、清单非法等）会删除该任务的 staging/backup 目录，允许用同一 `jobId` 重试。
- 发现残留的 `ota-in-progress` 标记且其 apply 进程已不存在（或属于上一次开机）时，新的 OTA 会先通过 `systemd-run --wait`
  执行 `ota-rollback.sh --recover-interrupted` 完成恢复再继续；恢复失败才拒绝。OTA 安装 `gateway-ota-recover.service` 时会自动 `enable`。
- 健康检查中 `activating`（非 `auto-restart`）视为正常，`ky-ems`/`local-kiosk@` 不参与判定；回滚结果写入
  `upgrade_history.log`（`result=rollback|rollback-failed`），并恢复升级前的 `current_version.txt`/`applied_version.txt`。
