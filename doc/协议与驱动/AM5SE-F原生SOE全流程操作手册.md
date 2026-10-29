# AM5SE-F 原生 SOE 全流程操作手册

适用：`realese1.0` 中的 AM5SE IEC103 驱动与 SOE 历史页，功能来源提交 `bffcafc`。本文说明实际实现，不将标准协议的全部能力视为已实现或已验证。

阅读顺序：第 2 节看总流程，第 3～4 节看怎么读和拆报文，第 5 节看怎么组装，第 6 节看怎么存，第 7 节看怎么显示，第 8～14 节按现场操作和排障执行。

## 1. 先分清三个概念

| 数据 | 含义 | 本功能如何处理 |
| --- | --- | --- |
| 当前点值 | 此刻或最近一次采到的状态、遥测 | 仍走原有点表、共享内存和采样链路 |
| 原生 SOE | 装置生成的带时标突发事件 | 从 ASDU1/2 中直接解析，单独保存为事件历史 |
| 故障录波 | 一段波形及其配置文件 | 使用录波目录和 COMTRADE 流程，不是 SOE 表里的事件行 |

**不是比较两次点值，再补一个 EMS 时间冒充装置 SOE。** 同样，收到 ASDU44 总召快照、ASDU10 遥测或 ASDU23 录波目录，都不代表收到新的 SOE。

163 在 2026-10-29 已完成实机连接验证：两分钟收到 381 帧遥测，未收到真实 ASDU1/2 COT1，事件库为 0 条。因此通信已验证，真实事件名称、DPI 极性和时标对照仍待现场验收。详见 [实机记录](../releases/am5-soe-163-real-20261029.md)。本文中的报文示例是教学数据，不是现场故障。

## 2. 完整数据流

```text
AM5 产生事件，保留装置时标
    |
既有独占 IEC103 TCP 连接
    | 链路初始化/总召；之后二级轮询，按 ACD 继续一级取数
readFrames：TCP 字节流拼成 FT1.2 帧并校验
    |
captureSoe：检查链路地址，筛选 ASDU1/2、COT=1
    |
decodeIec103Soe：取 CA/FUN/INF、DPI、CP32、RET/FAN
    |
分配 event_id，保留 EMS 接收时刻和原始编码
    |
describeIec103Soe：补机器/设备编号、点名、已确认的中文含义
    |
SoeStore::append：检查空间 -> SQLite 事务 -> 提交
    |
soe_events 历史库（独立于采样库）
    |
EMS SOE 页：按机器过滤、按入库序号倒序、只读分页
```

原始帧同时可以由被动 PCAP 留证。SOE 库的 `raw_hex` 是链路用户数据，不包含 FT1.2 外层起止符、长度和校验字节，不能替代完整 PCAP。

## 3. 怎么读：沿用现有通信链路

### 3.1 谁连接谁

- `am5se_active_tcp`：AM5 是 TCP Server，边端主动连接装置。163 当前使用此模式，装置为 `192.168.22.191:7710`。
- `am5se_passive_tcp`：边端监听，装置主动反连。只有装置本身按反连方式配置时才选它。
- 两种模式使用相同的 IEC103 事件处理链路。不要为了读 SOE 再开第二个 AM5 客户端，也不要把其他现场地址直接套过来。

“突发”指事件由装置产生，不等于本实现无轮询地接收网络推送。当前 AM5 链路通过 IEC103 一级/二级数据交换取数，事件可能在普通取数、目录或录波应答中穿插返回。

### 3.2 驱动的读取顺序

1. 建立或恢复既有 TCP 连接，初始化链路。
2. 首次执行总召，再取一级数据，直到总召结束、ACD 清除或达到本轮限额。
3. 常规 `poll()` 先请求二级数据，查看应答中的 ACD（有一级数据待取标志）。
4. ACD 置位时继续请求一级数据，受 `maxPollFrames` 限制，避免一轮无限取数。
5. `exchange()` 和 `exchangeWithFrameCount()` 收到有效帧后均调用 `captureSoe()`。因此录波链路穿插收到的 SOE 不只依赖普通采样路径处理。
6. 轮询、目录和录波操作由同一客户端的 `operationMutex_` 串行化，共享同一连接。超时重发使用相同 FCB，当前最多追加重发两次；这不是再次下发电气控制命令。

事件的取回延迟受调度、链路等待、装置队列、录波占用和落库耗时影响。CP32 含毫秒字段，不代表 EMS 必然在毫秒内收到事件，更不代表 PCS 的内部采样率。

### 3.3 TCP 怎么拼成 103 帧

`recv()` 的一次返回可能只有半帧，也可能包含多帧。`readFrames()` 将字节追加到 `rxBuffer_`：

1. 找 `0x10` 固定帧或 `0x68` 可变帧起始符。
2. 固定帧总长为 `4 + linkAddressSize`；可变帧检查两份长度相同、第四字节为 `0x68`，总长为 `L + 6`。
3. 字节不足时继续保留缓冲区；凑齐后取出完整帧，检查结构和校验和。
4. 有效帧进入协议处理；错误结构或校验失败的帧不作为有效事件入库。排障需要看 PCAP，而不是假设每个被丢弃帧都会有专门的 SOE 错误记录。

当前 SOE 解码器只接受单信息体 ASDU1/2，要求 `VSQ & 0x7f == 1`、长度符合实现预期，且 **COT 字节精确等于 1**。不能自行把带扩展标志或其他 COT 的厂家报文归类为已支持事件。

## 4. 怎么拆：一帧教学报文

以下为链路地址长度 1 的 ASDU1 示例。**仅用于离线阅读，不要发送到真实装置。**

```text
68 0e 0e 68 28 01 01 81 01 01 01 64 02 7b 00 00 0a 00 99 16
```

外层：`68 L L 68 ... CS 16`，`L=0x0e=14`；链路用户数据从 `28` 到末尾的 `00`。这 14 字节求和取低 8 位为 `0x99`。

以下偏移相对于链路用户数据，而不是整帧：

| 偏移 | 字节 | 含义 |
| --- | --- | --- |
| 0 | `28` | 链路控制字，含 ACD 标志 |
| 1 | `01` | 链路地址 1，须匹配实例配置 |
| 2 | `01` | ASDU1：带时标报文，含 DPI 双点状态 |
| 3 | `81` | VSQ，低 7 位为 1 个信息体 |
| 4 | `01` | COT=1：突发事件 |
| 5 | `01` | CA=1，公共地址 |
| 6 | `01` | FUN=1，功能类型 |
| 7 | `64` | INF=100，信息序号 |
| 8 | `02` | 原始 DPI=2；还不能据此断言“动作”或“合位” |
| 9..12 | `7b 00 00 0a` | CP32：10:00:00.123，毫秒字段小端排列 |
| 13 | `00` | ASDU1 尾部附加信息 SIN；当前无独立数据库列，随 `raw_hex` 保留 |

ASDU2 在 DPI 后增加 2 字节 RET、2 字节 FAN，再接 4 字节 CP32。当前代码按小端读取 RET/FAN，分别保存到 `relative_ms` 和 `fault`；ASDU1 没有这两项时为 `-1`，不是故障序号 0。

示例只证明字节如何拆，`FUN=1,INF=100,DPI=2` 的实际业务含义仍需核对装置型号、固件及现场状态。

## 5. 怎么组装：从编码到可读事件

### 5.1 一条事件的组成

| 信息来源 | 事件字段 | 用途 |
| --- | --- | --- |
| 生效的驱动身份配置 | `machineCode`,`deviceCode` | 归属哪台 EMS、哪个设备，不用 IP 当设备编号 |
| 原始 ASDU | `commonAddress,typeId,cause,functionType,informationNumber` | 定位事件来源，匹配规则 |
| 原始 DPI | `rawDpi`,`dpi`,`valueValid` | 原值保留；`dpi=rawDpi & 3`；只有原始值恰为 1 或 2 才有效 |
| 原始 CP32 + 接收时刻 + 偏移 | `rawTimeHex`,`deviceTimeMs`,`timeValid` | 保留时标及推定的绝对时间 |
| EMS 系统时钟 | `receivedMs` | 记录 EMS 收到并处理帧的时刻，不是装置动作时刻 |
| 客户端会话 + 自增序号 | `eventId` | 同一待提交事件重试时保持不变，不是平台 `cmdId` |
| 点表和已确认映射 | `name`,`state`,`mapped` | 生成中文事件名及含义，不能靠猜测补齐 |
| 原始链路用户数据 | `rawHex` | 事后核查、追溯，保留未知含义和无效质量 |

### 5.2 名称与 DPI 极性

描述规则依次为：

1. 默认名称是 `ASDU n FUN n INF n`，默认状态为 `DPI=n`。
2. 若现有点表有匹配 FUN/INF/CA 的可读点，可以借用点名。**点名匹配不代表已确认状态极性。**
3. 查找精确匹配 `(CA, ASDU类型, FUN, INF)` 的 `soe.mappings` 项。
4. 有明确映射且原始 DPI 有效时，DPI=1 使用 `state1`，DPI=2 使用 `state2`，标记 `mapped=true`。
5. 原始 DPI=0/3 或带非预期高位时保留原值并标记无效，不能因为配置了映射就变成有效事件。

假设在独立试验环境中确认某信号 DPI=2 是动作、DPI=1 是返回，才能将相应 `state2` 填“动作”、`state1` 填“返回”。这不是所有 AM5 固件的默认规则，更不能直接套成断路器分合状态。

每条映射需要的字段：

| 字段 | 填写要求 |
| --- | --- |
| `commonAddress` | 实际 CA，整数 0..255 |
| `typeId` | 实际 ASDU，1 或 2；相同 FUN/INF 的两种 ASDU 分开配置 |
| `functionType`,`informationNumber` | 实际 FUN、INF，整数 0..255 |
| `name` | 经核对的事件名称 |
| `state1`,`state2` | 分别是原始 DPI=1/2 的实际含义，均不可为空 |
| `confirmed` | 只有完成固件资料/现场状态核对后才设为 `true` |

四元组重复、缺字段、`confirmed` 未确认或越界会导致配置加载失败。没有确认依据时使用空数组 `mappings: []`，不要放一条未完成的映射进去凑数。

**映射在接收时生效，名称和状态随后写入数据库。修改配置并重启后只影响新接收事件，不会自动重算旧记录。** 旧事件需要再解释时，另存分析结果并关联原事件 ID，不批量覆盖原始证据。

### 5.3 时间如何组装

CP32 没有年月日。代码按 `utcOffsetMinutes` 将接收时间换算为装置当地日期，组合 CP32 的时分秒毫秒，再在当天和相邻日期之间选择离接收时刻最近的候选。

例如教学事件在当地 2026-10-29 10:00:00.200 被接收，CP32 为 10:00:00.123，偏移为 +480 分钟，则推定装置时间是 `2026-10-29T02:00:00.123Z`，接收时间为 `2026-10-29T02:00:00.200Z`。

页面显示 UTC，因此北京时间现场对照时要加 8 小时。时标字段不合法或带无效标志时，`time_valid=0`，页面显示 `--` 并保留原始字节。跨午夜可推定日期，但装置时钟错误、积压超过半天、夏令时等情况仍可能推错，不能将“日期推定”当成已对时。

## 6. 怎么存：独立 SQLite 事件库

默认路径：`/opt/modbus-gateway/data/iec103-soe.db`，表名 `soe_events`。不是 `point_samples_*.db`，也不是共享内存的“最新点值”。

### 6.1 提交顺序

1. 驱动启用 SOE 时创建 `SoeStore`，安装事件回调 `setSoeSink()`。
2. 每个解码事件分配“随机会话标识:递增序号”的 `event_id`，放入 `pendingSoe_`。
3. 回调补齐可读描述，调用 `append()`。单批上限 256 条；实际捕获路径通常逐帧提交，不会等待凑够 256 条。
4. 检查记录合法性和磁盘写入预算，至少预留 256 MiB 及估算写入空间。
5. 使用 WAL、`synchronous=FULL`、100ms SQLite busy timeout，执行 `BEGIN IMMEDIATE`。
6. 用参数绑定执行 `INSERT OR IGNORE`，全部成功后 `COMMIT`；事务内失败则尝试 `ROLLBACK` 并上报错误。
7. 回调正常返回后才清空内存待提交队列。下一次发送协议请求前还会先尝试提交未完成事件。

`event_id` 有唯一约束，同一批次落库重试不会重复插入；不同接收次数即使时间、内容相同也有不同 ID，不能将此称为装置到 EMS 的全局严格去重。

### 6.2 数据库列速查

| 数据库列 | 含义 |
| --- | --- |
| `id` | 自增入库序号；历史分页和默认显示顺序依据 |
| `event_id` | 此次接收的事件标识，用于本进程待提交重试幂等 |
| `machine`,`device` | EMS 身份、设备编号 |
| `name`,`state` | 接收时生成的名称、状态文本 |
| `ca`,`type`,`cause`,`fun`,`inf` | 原始事件定位字段 |
| `dpi`,`raw_dpi` | 低 2 位值、完整原始 DPI |
| `received_ms`,`device_ms` | 接收时刻、推定装置时刻，UTC epoch 毫秒；使用装置时刻前须看 `time_valid` |
| `relative_ms`,`fault` | ASDU2 的 RET、FAN，ASDU1 缺项为 -1 |
| `value_valid`,`time_valid`,`mapped` | 值是否有效、时标是否有效、状态是否经映射 |
| `raw_hex`,`raw_time` | 链路用户数据、原始 CP32 的十六进制文本 |

索引为 `(machine,id)` 和 `(received_ms,id)`。按 EMS 接收时间保留约 30 天，成功追加后至多每分钟清理最多 512 条过期记录。空闲时不主动清理，数据库文件不自动缩小，这不是严格的容量上限。

### 6.3 写库失败会怎样

磁盘不足、库不可写、库忙等失败会保留当前进程内待提交事件并阻止后续发送继续越过该批次；采集和录波可能因此受阻。先处理存储问题、保留日志，不以反复重启作为解决方法。

接收后提交前若进程崩溃或掉电，内存事件可能丢失；装置队列也可能溢出。WAL/FULL 和事件 ID 不能保证端到端零丢失、恰好一次。没有完整事件流证据时不能宣称“一条不漏”。

## 7. 怎么显示：从数据库读，不从瞬时点值拼历史

1. 带新组件的工程通过 `qtSoeTable` 创建 `SoeHistoryView`，使用画面的 `machineCode` 过滤记录。
2. 组件 `soeDatabase` 属性指定路径，未设置时使用默认 SOE 库。驱动写库路径和画面读库路径必须一致。
3. 只读打开库并设置 `query_only`；查询 `machine=?`，按 `id DESC` 排序，下一页加 `id < 上页末尾ID`。
4. 一次读 51 条以判断是否还有下一页，实际显示 50 条。最新页可见时每 2 秒刷新，历史页不强行跳动。
5. 入口是“告警 → SOE”；显示事件、状态、装置 UTC 时间、接收 UTC 时间和质量。悬停看 CA/FUN/INF/DPI、RET/FAN 和原始编码。

“暂无装置 SOE 事件”表示当前机器查询结果为空；“SOE 数据不可用”表示库打开或读取失败，两者不同。库中有数据但页面为空时先检查机器身份、数据库路径及显示程序/工程版本。

163 此次只部署了兼容版驱动，没有升级生产显示程序，不能因为写库已启用就断言现场已出现 SOE 页。现有功能也不包含平台历史 API、平台事件上报、页面日期筛选或一键导出。

## 8. 现场操作：先只读检查

下面地址、实例和路径是 **163 本次记录**，其他站先按实际配置替换。命令在目标 Linux 边端执行，不在 Windows PowerShell 中直接执行；不包含 SSH 密码。

```bash
ssh root@192.168.22.163
```

先看身份、服务和连接，身份应为 `GW_FACTORY_001`。若不一致，停止操作：

```bash
cat /opt/modbus-gateway/config/runtime/device_identity.json
systemctl show iec-driver@device_am5se.service -p ActiveState -p SubState -p MainPID -p NRestarts -p ExecStart
ss -ntp
```

确认到 `192.168.22.191:7710` 只有预期常驻 IEC 进程占用。不要用另一个驱动、调试助手或主动 TCP 探针竞争连接。

配置、磁盘、时间和日志只读检查：

```bash
python3 -m json.tool /opt/modbus-gateway/config/runtime/devices/device_am5se.json
df -h /opt/modbus-gateway/data
timedatectl status
journalctl -u iec-driver@device_am5se.service -n 100 --no-pager
stat /dev/shm/gateway_point_store_am5se_f
```

`json.tool` 仅验证 JSON 语法，**不是驱动语义校验**。不要运行 `IecDriver --once` 充当“配置检查”，它会真实连接和采集。

先确认上面的共享内存文件存在且 ABI 与 `pointctl` 匹配，再执行；文件不存在时不要用工具去“试着创建”：

```bash
/opt/modbus-gateway/bin/pointctl stats --shm gateway_point_store_am5se_f
/opt/modbus-gateway/bin/pointctl dump --shm gateway_point_store_am5se_f --limit 20
/opt/modbus-gateway/bin/pointctl pending-peek --shm gateway_point_store_am5se_f --limit 10
```

使用 `pending-peek`，不要换成会消费队列的 `pending`。163 的预期是 6 点可读、3 个遥测持续更新、`pending_count=0`、`writeSeq=0`；遥信时间戳可停留在最近一次总召或变位，不以时间戳未刷新就制造事件。

## 9. 首次启用、变更映射和部署

163 已启用 SOE，无需再次部署才能执行第 8、10 节只读检查。其他现场按以下顺序处理：

1. 确认型号、固件、装置通信角色、现有独占实例、机器/设备身份、维护授权。
2. 备份原驱动、原配置、服务启动信息、共享内存 inode/ABI、连接和点值基线；已有 SOE 库用第 11 节的一致快照备份，记录 SHA256。
3. 核对发布包 ABI。主分支 v11 二进制不能直接放到 163 的 v7 共享内存环境。需经测试的兼容包，或另行批准整套配套升级，不能只凭架构同为 ARM 就替换。
4. 在设备配置的 `protocol.iec` 中合并下列字段，不替换整个配置，不改变点表、身份、传输模式、网络或电气写权限。
5. 确保数据目录存在、服务账户可写，显示账户可读库及必要的 WAL/SHM 文件，板上有 `libsqlite3.so.0`，空间满足写入预算。
6. 在维护窗口内，按已审查部署流程仅停止目标 IEC 实例、核对退出、替换经校验的产物/配置，再启动同一实例。不能仅在常驻驱动旁运行新程序。部署流程必须具有可执行的回退方案。
7. 核对新进程实际可执行文件哈希、只有一条装置连接、点值质量、待写数、自动重启数、共享内存 inode 和其他服务基线。
8. 执行第 10、12 节。只有看到并对照真实事件后才确认含义映射；变更映射需要下一次获准维护窗口，配置不是热加载。

合并到 `protocol.iec` 的 SOE 配置片段：

```json
{
  "soe": {
    "enabled": true,
    "sqlitePath": "/opt/modbus-gateway/data/iec103-soe.db",
    "utcOffsetMinutes": 480,
    "mappings": []
  }
}
```

`480` 仅适用于装置按 UTC+8 计时的现场，不取决于操作员电脑所在时区。映射完整要求见第 5 节。

旧版入口可能在启动时清理“无注册写入者”的共享内存，即使读者还在。163 首次升级曾触发 inode 守卫并回退，修复兼容入口后才通过重启保留检查。**不要删除共享内存、放宽守卫或临时重启其他服务来掩盖这个问题。**

## 10. 直接查事件：只读、按机器过滤

在边端运行下列命令，输出当前机器总条数和最新 50 条。文件不存在会失败，不会悄悄新建空库。没有 `sqlite3` 命令行程序时可使用板上 Python3 标准库。

```bash
python3 - /opt/modbus-gateway/data/iec103-soe.db GW_FACTORY_001 <<'PY'
# am5-soe-readonly-query
from contextlib import closing
import json
from pathlib import Path
import sqlite3
import sys

path = Path(sys.argv[1]).resolve(strict=True)
machine = sys.argv[2]
with closing(sqlite3.connect(path.as_uri() + '?mode=ro', uri=True, timeout=1)) as db:
    db.row_factory = sqlite3.Row
    db.execute('PRAGMA query_only=ON')
    count = db.execute('SELECT count(*) FROM soe_events WHERE machine=?', (machine,)).fetchone()[0]
    rows = [dict(row) for row in db.execute(
        'SELECT * FROM soe_events WHERE machine=? ORDER BY id DESC LIMIT 50', (machine,))]
print(json.dumps({'machine': machine, 'count': count, 'rows': rows}, ensure_ascii=False, indent=2))
PY
```

判读顺序：先核对 `machine/device`，再看 `type/cause/fun/inf/raw_dpi`，然后看 `mapped/value_valid/time_valid`，最后解释名称和时刻。0 条只表示当前库中该机器没有记录，不证明链路异常，也不证明 SOE 已验收。

要看下一页，用上一页最后一条的 `id` 作边界，SQL 为：

```sql
SELECT * FROM soe_events
WHERE machine = ? AND id < ?
ORDER BY id DESC LIMIT 50;
```

保持参数绑定，不拼接不可信的设备编号；不要在现场库执行 INSERT、UPDATE、DELETE、VACUUM 或修改 journal_mode 来“排障”。

## 11. 怎么导出并核验

生产正在使用 WAL，不能只复制正在变化的 `.db` 主文件，也不要删除 `-wal`、`-shm`。使用 SQLite backup API，从只读源库生成一致副本。下面只写新的诊断文件，不改源数据。

先确认诊断目录存在、空间足够；以下命令最多运行 30 秒。超时产生的 `.partial` 不算成功证据，不要当完整数据库交付：

```bash
timeout --signal=TERM --kill-after=5s 30s python3 - /opt/modbus-gateway/data/iec103-soe.db /opt/modbus-gateway/diagnostics <<'PY'
# am5-soe-consistent-export
from contextlib import closing
import hashlib
import json
import os
from pathlib import Path
import sqlite3
import sys
import tempfile

source = Path(sys.argv[1]).resolve(strict=True)
folder = Path(sys.argv[2]).resolve(strict=True)
if not folder.is_dir():
    raise SystemExit('Export destination is not a directory')
fd, name = tempfile.mkstemp(prefix='am5-soe-', suffix='.db.partial', dir=str(folder))
os.close(fd)
partial = Path(name)
with closing(sqlite3.connect(source.as_uri() + '?mode=ro', uri=True, timeout=1)) as src:
    src.execute('PRAGMA query_only=ON')
    with closing(sqlite3.connect(str(partial), timeout=1)) as dst:
        src.backup(dst, pages=128, sleep=0.05)
        check = dst.execute('PRAGMA integrity_check').fetchall()
        if check != [('ok',)]:
            raise RuntimeError(check)
        count = dst.execute('SELECT count(*) FROM soe_events').fetchone()[0]
final = partial.with_suffix('')
if final.exists():
    raise RuntimeError('Refusing to overwrite an existing export')
partial.rename(final)
digest = hashlib.sha256()
with final.open('rb') as stream:
    for block in iter(lambda: stream.read(1024 * 1024), b''):
        digest.update(block)
print(json.dumps({'file': str(final), 'rowsAllMachines': count, 'bytes': final.stat().st_size,
                  'sha256': digest.hexdigest()}, indent=2))
PY
```

只有命令正常退出并输出最终 `.db` 路径、行数和 SHA256 才算导出完成。记录该输出，用 `scp` 下载**输出的准确文件名**，不要用模糊通配符拉错文件。在 Windows 用 `Get-FileHash -Algorithm SHA256 -LiteralPath '下载后的绝对路径'` 重开计算并与边端输出比较，再对副本运行第 10 节查询。副本内保留原始编码、质量和时间，不仅导出中文描述。

实际教训：编写本手册时发现早先模拟库的主文件独立读取只有 4 条，板上连同 WAL 读取为 8 条。已用以上脚本补取 163、22.7 的一致模拟库，各自下载后重开均为 8 条并通过哈希检查。**文件 SHA256 相等只证明传输未改变该文件，不证明遗漏 WAL 的主文件是完整数据库。**

## 12. 现场验收：报文、数据库、装置三方对应

1. 记录验收前数据库最大 `id`、服务 PID、设备固件和实际时区，确认无待执行控制命令。
2. 获准后在现有网卡上被动抓包；不新建到装置的 TCP 连接。抓包文件使用新目录，并设置时间/包数上限。
3. 优先观察自然事件。需要制造已知变位时，由有资质的现场人员在独立批准且安全的试验回路操作；本流程不提供分合闸、跳闸、复位或保护注入命令。
4. 同步记录装置面板/厂家工具的事件名称、动作/返回、装置时间和故障序号。
5. 按 TCP 流重组抓包，校验 FT1.2，再确认 ASDU1/2、COT、CA/FUN/INF、DPI 和 CP32。不要把 TCP 包数当事件数；考虑 TCP 重传和协议重发。
6. 对照新增数据库行的原始字段、接收时刻、质量及条数，不能只看中文显示。快速动作和返回应各有记录，未知/无效信息应如实保留。
7. 核对 UTC 与装置时区。ASDU2 再核对 RET/FAN；ASDU1 不要求不存在的字段。
8. 保存抓包、抓包统计、库一致副本、日志、配置/二进制哈希及现场记录，生成验收结果。无事件时只能标记“未覆盖事件验收”。

163 的有界抓包示例，网卡 `ens1` 必须先与本站核实：

```bash
CAPDIR=$(mktemp -d /opt/modbus-gateway/diagnostics/am5-soe-capture.XXXXXX)
timeout --signal=INT --kill-after=5s 120s tcpdump -i ens1 -nn -s 0 -U -c 10000 \
  -w "$CAPDIR/am5.pcap" 'host 192.168.22.191 and tcp port 7710'
```

`timeout` 到期通常返回 124，不等于抓包一定损坏；检查 tcpdump 退出统计、内核丢包数、文件可重开和帧重组完整性。抓包也可能因包数上限提前结束。不要只凭文件存在判为完整证据。

## 13. 排障与回退

| 现象 | 优先核查 | 不要做什么 |
| --- | --- | --- |
| 遥测正常、SOE 为 0 | 抓包是否真的有 ASDU1/2 COT1，SOE 是否启用 | 不制造假事件，也不把总召/录波目录算 SOE |
| 有事件但显示 DPI 或“含义待确认” | 四元组、实际固件极性、映射加载时间 | 不直接套用通用分/合含义 |
| 数据库有行、画面没行 | 生效 machineCode、路径、显示二进制及工程版本 | 不改机器身份强行显示别站数据 |
| 显示“SOE 数据不可用” | 文件/目录权限、WAL/SHM 可读性、SQLite 动态库、库结构 | 不以新建空库替换原库 |
| 事件时刻跨天或差 8 小时 | CP32 原值、UTC 显示、固定偏移、设备对时与积压 | 不凭接收时间覆盖装置原始时间 |
| 写库报错且采集受阻 | 空间预算、权限、库忙、日志；尽快保留待提交进程 | 不盲目重启，不删 WAL |
| 升级后段被替换或 ABI 不符 | 发布包 ABI、入口是否清段、inode 守卫 | 不删除段或放宽守卫继续运行 |
| 历史记录没随新映射改名 | 本实现按接收时含义存储，属于预期 | 不覆盖旧证据制造“全部已确认” |

需要回退时：先保存日志及可读的一致库副本；按批准的维护流程仅停止本次 IEC 实例，核对退出，再恢复配套备份的驱动和配置，校验哈希并启动原实例。事件数据库及 PCAP 保留，不用备份库覆盖新事件。最后检查独占连接、点质量、pending、配置、共享内存和无关服务。旧驱动也可能存在启动清段行为，回退不天然等于原始运行数据全部恢复；无法通过核验时停止扩大操作并报告。

## 14. 离线模拟的安全边界

- 网络 namespace 只能隔离网络，不能隔离宿主 `/dev/shm`。`backend: memory` 也不能保证直接使用 `MemoryPointStore` 的入口不打开共享段。
- 必须使用独立 mount/net/ipc namespace，在确认不属于宿主 mount namespace 后将 `/dev/shm`、`/run`、`/tmp` 挂私有 tmpfs；设置独立段名、SQLite 路径、身份和录波 socket，网络只开回环。
- 测试前后核对宿主关键进程、配置和共享内存，不在生产环境直接跑 `IecDriver --once` 或未隔离的测试二进制。
- 不要复用已禁用的早期诊断入口。已有模拟残留必须先界定范围、保存快照并取得清理授权，不能直接清空默认段。
- 模拟协议通过、空库可读和真实事件三方对照，是三个不同验收等级。

## 15. 代码入口与交付清单

| 环节 | 代码入口 |
| --- | --- |
| 配置合法性和映射检查 | [src/config_loader.cpp](../../src/config_loader.cpp)：`parseIecProtocol` 所在 IEC 配置解析段 |
| 连接、拼帧、ACD/FCB、捕获和失败重试 | [src/am5se_iec103_client.cpp](../../src/am5se_iec103_client.cpp)：`poll/readFrames/captureSoe/flushSoe` |
| ASDU 解码、日期推定、中文描述 | [src/iec103_soe.cpp](../../src/iec103_soe.cpp)：`decodeIec103Soe/describeIec103Soe` |
| 内存事件字段 | [include/edge_gateway/iec103_soe.hpp](../../include/edge_gateway/iec103_soe.hpp)：`Iec103SoeEvent` |
| 回调组装和绑定历史库 | [iec_driver_main.cpp](../../iec_driver_main.cpp)：`setSoeSink` |
| 事务、幂等、保留和分页 | [src/soe_store.cpp](../../src/soe_store.cpp)：`SoeStore::append/read` |
| 画面读取和分页 | [local_display_qt_soe.hpp](../../local_display_qt_soe.hpp)：`SoeHistoryView` |
| 工程组件绑定 | [local_display_qt_scada_scene.cpp](../../local_display_qt_scada_scene.cpp)：`qtSoeTable` |

现场交付至少包含：型号/固件与身份、发布和配置哈希、ABI 基线、独占连接证明、事件地址/极性确认表、带原始字段的库副本与 SHA256、PCAP 与丢包统计、装置侧事件记录、未覆盖项及回退记录。

参考：[实现说明](AM5SE-F原生SOE.md)、[22.7 模拟记录及更正](../releases/am5-soe-22-7-20261029.md)、[163 实机记录](../releases/am5-soe-163-real-20261029.md)。历史主动 TCP 验证文档中的独立进程命令不适用于已有常驻实例的直接并行执行；录波另走既有实例本地 socket 任务。

## 16. 本手册的验证范围

查询脚本已用生产零事件快照、早期不完整模拟主文件及两份补取的完整模拟快照验证：按机器过滤有效，查错机器返回 0，源文件不存在时失败且不创建空库。导出脚本已在 163、22.7 的原模拟库上只读执行，并对本地副本再次验证查询、完整性、哈希和源文件未改动。教学帧校验和、字段、CP32 时间换算、JSON 片段及文内代码链接也已校验。

这些验证未重启现场驱动、未再次运行模拟器、未产生真实保护事件；不替代现场事件语义验收。校验记录：`D:/workspace/new/am5-soe-20261029/operation-guide-verification.json`；补取证据清单：`D:/workspace/new/am5-soe-20261029/consistent-simulated-exports/manifest.json`。

## 17. SOE 录波关联：从查询到显示

### 17.1 “有关联录波”不是突发事件中的通用布尔位

ASDU2 含故障序号 FAN，但 FAN 本身不证明装置仍保有文件、文件已生成或已经下载。ASDU1 没有 FAN，也不能据此判定该事件绝无录波。本实现将事件与 ASDU23 录波目录进行核对，保留证据及不确定性，不改写原始事件。

本节是新增能力，不能套用前述 163 真实通信验证结论。此前真实观察没有 ASDU1/2 突发事件，尚未对真实 SOE 和真实录波做成对验收。后续 163 v7 兼容部署、身份缺失回退及修复记录见 [现场部署记录](../releases/am5-soe-recording-163-20261029.md)；只升级了 IEC 驱动，没有升级旧画面。

### 17.2 配置与启用边界

在现有 `protocol.iec.soe` 对象中增加 `recordings`，保留原 `sqlitePath`、时区和映射：

```json
"recordings": {
  "enabled": true,
  "refreshIntervalMs": 30000,
  "directoryTimeoutMs": 2000,
  "matchWindowMs": 1000
}
```

两个仓库示例均为 `enabled:false`，避免升级后静默增加设备请求。启用录波关联要求 `soe.enabled:true`；刷新间隔允许 10000..600000 ms，目录操作超时允许 500..10000 ms，匹配窗口允许 0..5000 ms。参数随驱动启动加载，需要批准的维护窗口，不能为加载配置擅自重启现场。

启用前还必须确认身份文件提供非空 `machineCode`，设备配置顶层提供非空且本站唯一的 `meterCode`。两个示例已补设备编号示意值，现场需用本实例真实约定编号，不能让多台装置复用示例编号。缺少这两项时新驱动在建库和通信之前拒绝启动，不能等到每次目录入库才暴露身份缺失。163 首次部署遗漏了空 `meterCode`，目录入库报错并造成采集坏质量，已触发回退；后续部署记录必须如实保留这次失败。

`refreshIntervalMs` 是自动查询的最小间隔，不是永久定频轮询。注册回调时安排最多 3 次检查，收到新的 ASDU2 时重新给出 3 次检查预算；每次在正常 `poll()` 末尾、间隔到达后执行一次。没有新事件时预算耗尽即停止自动检查。它不能保证发现超过这段观察窗口才生成的文件，后续可走既有实例的显式目录查询。

目录查询、常规采集、SOE 接收和手动下载共用同一 socket 与 `operationMutex_`，不另建竞争连接。单次目录查询共用一个截止时间，含重试，并受 `maxPollFrames` 与目录记录循环边界限制；达到边界但尚未取得结束证据时标记不完整。查询期间会占用本实例通信时间，不能声称对采集周期毫无影响。错误向上层报告，不将失败当空目录。目录回调写库失败也会报错，不生成假的成功状态。

### 17.3 怎么读、怎么组装

1. 既有事件入口保存 ASDU1/2 COT1，ASDU2 保留 FAN、RET、CP32、装置推定时间和接收时间。
2. 驱动通过既有连接发送目录请求 ASDU24，处理直接回复或后续一级数据中的 ASDU23 COT31。目录期间穿插的突发事件仍走原 SOE 落库入口。
3. 校验链路地址、目录 CA、响应原因与精确长度，拒绝负确认/测试标志、截断或多余数据。保留每条目录的 FAN、状态原值、原始 CP56。相同 FAN、不同时间或不同状态不合并为一条。
4. 取得目录响应且 ACD 清除，才记为完整目录；仅有 ACK、超时或循环上限不能证明目录完整。
5. CP56 使用 `soe.utcOffsetMinutes` 转 UTC，不用宿主时区。无效标志、夏令时/非预期标志、非法日期都不参与有效匹配。原始字节仍保留。
6. 查询历史时，先隔离 `machine + device + CA`，再检查 FAN 及装置时间。只有唯一目录候选、时间差不超过 `matchWindowMs` 且目录查询完整成功，才显示“目录已关联”。

事件 CP32 没有日期，其日期仍按接收时刻推定。当前不假设“目录时间 = 事件时间 - RET”，也不擅自加减 RET；固件时间语义未确认前，时间对不上显示关联不确定。即使落入匹配窗口，仍是软件关联结果，不取代装置面板、报文和 COMTRADE 时间的现场对照。不得单纯放大窗口让验收变绿。

### 17.4 怎么存、怎么查询

原 `soe_events` 保持不变，同库增加三张附表：

| 表 | 内容与键 |
| --- | --- |
| `soe_recording_checks` | 每个 machine/device/ca 的最后检查时间、complete、success、window_ms |
| `soe_recording_directory` | 最新目录快照；键为 machine/device/ca/fan/raw_time/state；存 UTC time_ms 及原始状态/CP56 |
| `soe_recording_downloads` | 已完成且身份核验通过的下载；键为 machine/device/ca/fan/raw_time；存下载时间、CFG/DAT 路径和字节数 |

目录快照与检查结果在同一 SQLite 事务更新。它们是“最后一次观察”，不是每次目录查询的审计历史；需要完整线路过程时保留 PCAP/日志。下载凭据跨目录滚动保留，同一录波的再次下载更新这条凭据，但旧文件不自动删除。

`SoeStore::read` 在一致读事务内计算 `Iec103SoeEvent.recording`，状态不是 `soe_events` 中的永久布尔列。第 10 节原始 SOE 查询仍有效，但不会自动导出派生录波状态。需要完整复核时使用第 11 节 SQLite backup 导出整个库，并保留关联 CFG/DAT；只拷贝 SOE 表或运行中的主 `.db` 文件不足以保全这些信息。

对一致副本可用下面的 SQL 查看原始关联证据；`:machine/:device/:ca` 用绑定参数填写，避免跨设备混查：

```sql
SELECT checked_ms, complete, success, window_ms
FROM soe_recording_checks
WHERE machine=:machine AND device=:device AND ca=:ca;

SELECT fan, time_ms, state, raw_time
FROM soe_recording_directory
WHERE machine=:machine AND device=:device AND ca=:ca
ORDER BY fan, time_ms;

SELECT fan, time_ms, raw_time, downloaded_ms, cfg_path, dat_path, cfg_bytes, dat_bytes
FROM soe_recording_downloads
WHERE machine=:machine AND device=:device AND ca=:ca
ORDER BY downloaded_ms DESC LIMIT 200;
```

旧库可被新画面只读打开；无附表时显示待核实/不确定，不由显示进程创建或迁移。新写入端采用附加建表迁移，旧 SOE 数据和读取列不变。历史 SOE 仍沿用约 30 天渐进保留。目录仅留每设备最新快照；下载元数据和文件当前不自动过期、不随 SOE 清理删除，因此运维必须监控容量、先归档再按独立审批管理。写入沿用至少 256 MiB 空间余量准入，但这不是严格的数据库容量上限。

### 17.5 页面状态口径

| 内部状态 | 页面 | 判定含义 |
| --- | --- | --- |
| `pending` | 待核实 | 尚无可用检查；早于事件接收的空目录不能当否定证据 |
| `linked` | 目录已关联 | 唯一 FAN/时间候选，目录完整成功；不保证文件当前可取 |
| `not_found` | 当前目录未找到 | 事件接收后有完整成功目录且无该 FAN；不代表从未录波 |
| `uncertain` | 关联不确定 | 无 FAN、无效时间、目录不完整、FAN 复用或多候选等 |
| `query_failed` | 目录查询失败 | 最近检查失败且无目录候选可用；存在部分候选时保持不确定 |
| `downloaded` | 已下载 | 留有身份核验通过的下载凭据，CFG/DAT 仍存在且大小一致 |
| `download_missing` | 本地文件缺失或大小变化 | 已有下载凭据，但文件检查不通过 |

“录波详情”或双击录波单元格可看 FAN、目录检查时间、目录时标、原始状态/CP56、下载时间及文件路径。详情只读，不发下载或电气控制命令。目录状态字在厂家固件位定义确认前仅显示数值，不翻译成“有效/就绪”。

历史已下载记录在目录滚动后仍可显示已下载，但出现当前 FAN 歧义时会保守降为不确定。状态只反映已有证据；检查时间必须一起看。复制库到另一台机器后，原绝对路径未必存在，页面可能显示文件缺失，这不证明原设备上的文件丢失。

### 17.6 下载如何回填

没有自动下载。经现场允许后仍使用第 8 节及既有录波流程的本地 socket 任务，不启动第二个 IEC 驱动连接 AM5。启用关联后，显式下载会在传输前后各查询一次目录；两次都完整，且对应 FAN 都只有一条、原始 CP56 和状态一致、时标有效，才写入下载附表。

CFG/DAT 先写临时文件再完成文件落地；启用关联时使用 `FAN00015_<时间>_<随机数>.CFG/.DAT` 形式，降低故障号复用导致覆盖的风险。若前后目录变化、身份不唯一，文件即使已取回也不强行关联；后置目录查询失败会使任务报错，已经落地的文件保留供复核。再次操作前先检查任务回执、目录和现存文件，不能只按错误重复下载。

当前“已下载”只核对文件存在和字节数，不计算持续内容哈希，同大小被改写不能检出。归档交付仍需单独对 CFG/DAT 做 SHA256，并核对 COMTRADE 结构、通道和触发时间。

### 17.7 新增验收项

隔离模拟须覆盖：ASDU1 无 FAN、无效时间、唯一匹配、完整空目录、部分目录、超时、错误链路地址/CA、负确认、多余/截断字节、同 FAN 多候选、FAN 跨时段复用、下载中 FAN 变化、文件缺失、旧库只读及附加迁移。真实验收还须把同一次自然/独立批准试验事件的突发报文、目录条目、装置记录和 CFG/DAT 对上，确认 RET/CP32/CP56 的时间关系及目录状态字定义。

只出现 SOE、只出现目录或只有模拟通过，都不能写“实机录波关联通过”。本次实现及测试记录另见 [录波关联补充验证](../releases/am5-soe-recording-20261029.md)。
