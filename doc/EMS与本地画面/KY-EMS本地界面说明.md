# KY-EMS 本地界面说明

## 1. 当前定位

当前生产本地界面以独立 `KY-EMS` Qt 工程为准：

- 源码目录：`D:\workspace\Embedded\KY-EMS`
- 边端运行目录：`/opt/modbus-gateway/ky-ems`
- systemd 服务：`ky-ems.service`
- 编译教程：[192.168.22.11边端交叉编译教程.md](../交叉编译教程/192.168.22.11边端交叉编译教程.md)

边端仓库中的 `LocalDisplayQtEms` 只是早期 POC 和点位浏览方案，不再作为当前生产 EMS 画面的主入口。完整 EMS 画面迁移、打包和初始化包嵌入都以 `KY-EMS` 工程为准。

## 2. 数据绑定

旧 KY-EMS 画面主要通过：

```text
KY-EMS-Config.xml widget + appDataIndex -> QRamRT::GetItemValue(appDataIndex)
```

当前迁移目标是：

```text
meterCode + pointCode -> KY-EMS-PointMap.json -> 当前共享内存 index -> 界面控件
```

兼容顺序：

1. 优先使用 `meterCode + pointCode` 从 `PointMap` 查找当前运行 index。
2. 找不到时使用 `indexFallback`。
3. 再找不到时兼容旧 `appDataIndex`。
4. 最后使用控件默认值。

详细规则见：[KY-EMS点位绑定迁移说明.md](../需求说明/KY-EMS点位绑定迁移说明.md)。

## 3. 运行文件

当前 `ky-ems` 目录应包含：

```text
KY-EMS
KY-EMS-Config.xml
KY-EMS-PointMap.example.json
VarList.xml
dataStoreBases/
```

其中：

- `KY-EMS`：全志 aarch64 Qt 可执行文件。
- `KY-EMS-Config.xml`：界面控件绑定配置。
- `KY-EMS-PointMap.example.json`：当前工程点位映射。
- `VarList.xml`：旧工程兼容入口。

## 4. 当前已验证版本

2026-06-26 已验证：

- 编译机：`192.168.22.11`
- 运行测试机：`192.168.22.16`
- 二进制大小：`8867664` 字节
- SHA256：`47b0fa243c294b9613078b989c1bc220f3fdb7e23dd5f68625c46ae03ba75b35`

验证结果：

- `ky-ems.service` 可正常启动。
- `VarList.xml` 加载成功。
- `KY-EMS-PointMap.example.json` 加载成功。
- 当前二进制已同步到 Win 初始化包资源。

## 5. 部署检查

查看服务：

```sh
systemctl status ky-ems.service --no-pager
journalctl -u ky-ems.service -n 100 --no-pager
```

替换前建议备份：

```sh
mkdir -p /opt/modbus-gateway/backup/ky-ems
cp -a /opt/modbus-gateway/ky-ems/KY-EMS \
  /opt/modbus-gateway/backup/ky-ems/KY-EMS.$(date +%Y%m%d%H%M%S)
```

替换后重启：

```sh
systemctl restart ky-ems.service
systemctl status ky-ems.service --no-pager
```
