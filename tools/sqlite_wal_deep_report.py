#!/usr/bin/env python3
"""Generate a Chinese report from the isolated large-fixture validation results."""
import argparse
import csv
import json
from pathlib import Path
import sys


def percentile(values, fraction):
    values = sorted(values)
    return values[int((len(values) - 1) * fraction)] if values else 0


def summarize(stage):
    workers = stage["workers"]
    producers = [w for w in workers if w["role"] == "producer"]
    replay = next(w for w in workers if w["role"] == "replay")
    reader = next(w for w in workers if w["role"] == "reader")
    enqueue = [v for w in producers for v in w["latencyMs"]]
    failures = [v for w in workers for v in w["errorLatencyMs"]]
    elapsed = max(w["elapsedMs"] for w in workers) / 1000
    samples = stage["samples"]
    resumed = next((e["seconds"] for e in stage["events"] if e["kind"] == "replay-pause-end"), None)
    zero_sample = next((s["seconds"] for s in samples if resumed is not None and
                        s["seconds"] >= resumed and s.get("pending") == 0), None)
    post_resume = [s for s in samples if resumed is not None and s["seconds"] >= resumed and "pending" in s]
    result = dict(label=stage["label"], seconds=stage["seconds"], version=stage["opened"]["version"],
                  profile=stage["profile"], injected=stage["injected"], accepted=stage["accepted"],
                  acceptedPerSecond=stage["accepted"] / elapsed,
                  committedReplay=stage["committedReplay"], errors=stage["errorCount"],
                  enqueueP95Ms=percentile(enqueue, .95), enqueueP99Ms=percentile(enqueue, .99),
                  queryP99Ms=reader["p99Ms"], failureMaxMs=max(failures, default=0),
                  cpuOneCorePercent=sum(w["cpuSeconds"] for w in workers) / elapsed * 100,
                  rssHighWaterSumMiB=sum(w["rssMaxKiB"] for w in workers) / 1024,
                  walSamplePeakMiB=max(s["walBytes"] for s in samples) / 1024**2,
                  minAvailableMiB=min(s["memAvailableBytes"] for s in samples) / 1024**2,
                  minDiskFreeGiB=min(s["freeBytes"] for s in samples) / 1024**3,
                  maxPendingSample=max(s.get("pending", 0) for s in samples),
                  pendingBeforeDrain=stage["afterLoad"]["pending"],
                  pendingAfterDrain=stage["final"]["pending"],
                  drainEvents=stage["drain"]["events"],
                  replayApiCount=replay["events"], replaySends=replay["sends"],
                  callbackMinusCommitted=replay["sends"] - stage["committedReplay"],
                  replayPauseWaitMs=replay["pauseWaitMs"], dataValid=stage["dataValid"],
                  resumeToFirstZeroSampleSeconds=None if zero_sample is None else zero_sample - resumed,
                  pendingGrowthAfterResume=None if not post_resume else post_resume[-1]["pending"] - post_resume[0]["pending"],
                  openWallMs=stage["opened"]["openAndCloseWallMs"])
    return result


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("results", type=Path, help="downloaded result root containing deep and ENOSPC results")
    parser.add_argument("output", type=Path, help="Markdown report path")
    args = parser.parse_args()
    deep = json.loads((args.results / "deep/summary.json").read_text(encoding="utf-8"))
    smoke = json.loads((args.results / "smoke/summary.json").read_text(encoding="utf-8"))
    enospc = json.loads((args.results / "enospc-final-results/enospc.json").read_text(encoding="utf-8"))
    rows = [summarize(s) for s in deep["stages"]]
    assert deep["dataValid"] and smoke["dataValid"] and all(r["dataValid"] for r in rows)
    assert len(enospc) == 5 and all(e["valid"] and e["sameConnection"]["valid"] for e in enospc)
    seed = deep["seed"]
    durations = "/".join(str(r["seconds"]) for r in rows)
    injected_metrics = next(r for r in rows if r["injected"])
    recovery_seconds = injected_metrics["resumeToFirstZeroSampleSeconds"]
    recovery_text = "持续入队期间未采到积压归零" if recovery_seconds is None else f"恢复后约 {recovery_seconds:.2f} 秒首次采到积压为 0"
    title = "22.16 SQLite 大库、长读与空间耗尽恢复验证"
    lines = [f"# {title}", "", "日期：2026-09-07。设备：COMM202600999。", "",
             "## 结论", "",
             f"- 在约 {seed['fileBytes'] / 1024**3:.3f} GiB 合成历史库上完成三组运行，时长分别为 {durations} 秒；"
             f"新增成功入队 {sum(r['accepted'] for r in rows)} 条，调用失败 {sum(r['errors'] for r in rows)} 次。",
             "- 三组排空后待发均为 0；主表与增量统计一致，历史条数和抽样内容不变，最终完整性检查通过。",
             "- WAL/FULL 增加长读事务和暂停回放注入；验证恢复，不与未注入的两组直接作性能排名。",
             f"- WAL/FULL 负载结束尚余 {injected_metrics['pendingBeforeDrain']} 条待发，随后停止入队并额外排空。"
             "排空通过不能替代持续入队情况下的消积压能力。",
             "- 五种组合均通过私有 32 MiB tmpfs 的操作系统 ENOSPC 测试；空间释放后能重新入队及回放。",
             "- 旧版 3.31.1 + DELETE/NORMAL 复现原始磁盘满异常被覆盖；新版四组本轮未复现。不能把本报告解释为所有问题已解决。",
             "- 未升级系统 SQLite、未修改生产数据库模式、未替换生产驱动、未控制 PCS。本报告不是投产批准。", "",
             "## 测试对象与方法", "",
             f"- 历史行 {seed['rows']} 条，每条 payload 16384 字节，全部 sent=1；"
             f"库文件 {seed['fileBytes']} 字节，实际分配 {seed['allocatedBytes']} 字节，不是稀疏文件。",
             f"- 生成耗时 {seed['seconds']:.1f} 秒；首次完整性检查 {seed['signature']['integritySeconds']:.1f} 秒。",
             "- 此库不是 104 的真实备份；只模拟大体量已发送历史，不复现其点位、报文分布和原始索引碎片。",
             "- 真实当前 Outbox：两个入队进程，每批 16 条、每条 1 KiB、间隔 100ms，并更新事件状态；"
             "一个回放进程，一个每 5ms 查询 pending 的短读进程。",
             "- 回放回调仅模拟等待，不连接 MQTT broker；不代表真实 PUBACK、平台解析或网络吞吐。",
             "- 每阶段前后独立核对表行数、sent、pending、字节统计；负载期间只低频采样小型 stats 表。",
             "- 成功调用延迟和失败调用耗时分开。CPU 100% 为一个核心；RSS 为各工作进程 VmHWM 之和，不是同时峰值。",
             "- 入队 P95/P99：合并两个 producer 的完整 latencyMs 数组并升序排列，取从 0 开始的 floor((N-1)×p) 项；"
             "不是两个进程各自分位数的平均值或最大值。原始样本保存在各阶段 p0.json/p1.json，生成工具可复算。",
             "- 生成耗时与首次完整性耗时取 deep/seed.json 的 seconds、signature.integritySeconds；"
             "seed-progress.json 另保留灌入进度。报告速率和 CPU 分母为工作进程最大实际 elapsedMs，不是假定正好 300 秒。",
             "- WAL/积压/剩余内存记录间隔约 5 秒，可能遗漏短峰值。板厂内核缺少进程 I/O 记账，不报告写放大。",
             "- 根分区余量低于 4 GiB、WAL 超过 512 MiB、MemAvailable 低于 512 MiB 时中止测试并回收测试子进程。", "",
             "## 大库运行结果", "",
             "|模式|注入|成功入队|条/秒|批量入队 P95/P99 ms|查询 P99 ms|失败|排空前/后积压|",
             "|---|---|---:|---:|---:|---:|---:|---:|"]
    for r in rows:
        lines.append(f"|{r['version']} {r['profile']}|{'长读、暂停回放' if r['injected'] else '无'}|{r['accepted']}|"
                     f"{r['acceptedPerSecond']:.1f}|{r['enqueueP95Ms']:.2f}/{r['enqueueP99Ms']:.2f}|"
                     f"{r['queryP99Ms']:.3f}|{r['errors']}|{r['pendingBeforeDrain']}/{r['pendingAfterDrain']}|")
    lines += ["", "|模式|CPU 单核 %|工作进程峰值之和 MiB|WAL 采样峰值 MiB|最少可用内存 MiB|最少磁盘余量 GiB|",
              "|---|---:|---:|---:|---:|---:|"]
    for r in rows:
        lines.append(f"|{r['profile']}|{r['cpuOneCorePercent']:.2f}|{r['rssHighWaterSumMiB']:.2f}|"
                     f"{r['walSamplePeakMiB']:.2f}|{r['minAvailableMiB']:.0f}|{r['minDiskFreeGiB']:.2f}|")
    lines += ["", "同一个库顺序运行，且注入不同；缓存、文件增长和测试顺序均有影响，不把表格当成纯版本性能排名。", "",
              "## 长读与回放恢复", ""]
    injected = next(s for s in deep["stages"] if s["injected"])
    for event in injected["events"]:
        labels = {"long-read-start": "开始持有读快照", "long-read-release": "释放读快照", "replay-pause-start": "暂停回放", "replay-pause-end": "恢复回放"}
        lines.append(f"- {event['seconds']:.2f} 秒：{labels[event['kind']]}。")
        if "checkpoint" in event:
            lines.append("- 长读期间 PASSIVE checkpoint 原始返回（busy / 日志帧数 / 已检查点帧数）：`" + json.dumps(event["checkpoint"]["rows"]) + "`。")
    lines += ["", f"- 采样最大积压 {injected_metrics['maxPendingSample']} 条；{recovery_text}（采样精度约 5 秒）。",
              f"- 恢复后第一条到最后一条有效采样，积压净变化 {injected_metrics['pendingGrowthAfterResume']} 条。"
              "正值表示这一观察段仍在积压，不能判为恢复后已稳定追上入队。",
              f"- 活跃读事务下切 DELETE：rc={deep['liveSwitch']['rc']}，实际 journal 仍为 {deep['liveSwitch']['journalAfter']}。"
              "这验证了必须停掉所有使用者后再切换模式。",
              "- 暂停只在领取事务之前等待，不持有写事务；不能等同于真实网络断线、慢 PUBACK 或断网重连。", "",
              "## 崩溃与重开", "",
              "- ctypes 连接先提交一条探针，再保持另一条插入和历史行修改未提交，SIGKILL 后用真实 Outbox 重开。",
              "- 已提交探针保留，未提交插入和历史修改回滚，待发探针可排空。不是 Outbox 写 API 自身的强杀测试。",
              f"- 所有连接退出后，用系统旧库重开为 DELETE/NORMAL；耗时 {deep['rollback']['openAndCloseWallMs']:.2f}ms。",
              f"- 最终完整性 {deep['finalHistory']['integrity']}，用时 {deep['finalHistory']['integritySeconds']:.1f} 秒，历史 payload 总字节与抽样 SHA256 一致。",
              "- 这是进程崩溃恢复和旧库重开兼容验证，不是物理掉电、备份还原或维护窗口耗时保证。", "",
              "## 操作系统空间耗尽", "",
              "私有 mount namespace 中使用 32 MiB tmpfs，filler 写入得到 errno=28。主机根分区未填满。"
              "程序也自行校验独立挂载点、命名空间、tmpfs 类型、容量和初始空目录，误传宿主目录会被拒绝。", "",
              "|版本/模式|失败次数|错误阶段成功条数|释放空间后入队|最终待发|完整性|",
              "|---|---:|---:|---:|---:|---|"]
    for item in enospc:
        w = item["full"]
        lines.append(f"|{w['version']} {w['journal']}/{w['synchronous']}|{w['errors']}|{w['events']}|"
                     f"{item['recovered']['events']}|{item['final']['pending']}|{item['integrity']}|")
    lines += ["", "另保持同一个 Outbox 对象运行 2 秒，起初占满空间，约 0.6 秒后释放 filler，不重启入队进程：", "",
              "|版本/模式|释放前表行数|同连接失败次数|同连接恢复入队|排空后待发|",
              "|---|---:|---:|---:|---:|"]
    for item in enospc:
        same = item["sameConnection"]
        w = same["worker"]
        lines.append(f"|{w['version']} {w['journal']}/{w['synchronous']}|{same['beforeRelease']['rows']}|"
                     f"{w['errors']}|{w['events']}|{same['final']['pending']}|")
    lines += ["", f"单组最大 tmpfs 已用 {max(e['tmpfsUsedBytes'] for e in enospc) / 1024**2:.2f} MiB，"
              "每组完成校验后只清理该组的测试库及 sidecar，再进入下一组。"]
    lines += ["", "synchronous=1 为 NORMAL，2 为 FULL。该结果仅覆盖 tmpfs 的真实 ENOSPC 错误传播与释放后重开，"
              "以及上述同连接恢复测试，不是 eMMC/ext4 满盘、I/O 损坏或掉电安全证明。", "",
              "### 新发现：原始异常被覆盖", "",
              "本轮仅旧版 3.31.1 + DELETE/NORMAL 的五次失败报成保存点不存在，新版四组均正确报磁盘满。"
              "写入空间耗尽可能使 SQLite 自动回滚整个事务，保存点随之消失。"
              "`enqueueBatchAndStates` 的 catch 继续执行可抛异常的 `ROLLBACK TO mqtt_target_0`，"
              "导致原始磁盘满异常被 `no such savepoint: mqtt_target_0` 覆盖。", "",
              "建议修复时保留原异常，检测事务是否仍活动；整个事务已回滚时不得把 optional target 当作局部失败后继续提交。"
              "需要针对主目标、可选目标、状态同事务分别补回归。本轮不修改生产业务实现。", "",
              "## 投递语义", "",
              "|模式|数据库确认 sent|回放 API 计数|模拟发送回调条数|回调减确认|",
              "|---|---:|---:|---:|---:|"]
    for r in rows:
        lines.append(f"|{r['profile']}|{r['committedReplay']}|{r['replayApiCount']}|{r['replaySends']}|{r['callbackMinusCommitted']}|")
    lines += ["", "上述为负载阶段，排空另计。API 抛错可能不返回已经提交的前缀数量；发送成功而 ACK 未落盘可能重发。"
              "数据库统计用于确认结果，平台仍需 event_id 幂等，不承诺恰好一次。", "",
              "## 证据与复测", "",
              "- 交叉编译：22.11 独立源码快照，GCC 6.3.1，全志 AArch64。",
              "- 私有 SQLite SHA256：`b0ce2f22de6476e5ee71b98986ebddfc0fab8e4fb4aa5544d1e1b04a37c2a6ed`。",
              "- 测试程序 SHA256：`5c7e9dfcab16ce2ffafb81000776a9c6ea28d09582e307b67899f0dd08b7e16a`。",
              "- 原始资料：`artifacts/builds/sqlite-wal-deep-20260907/`，包含小库、大库、ENOSPC、保护校验、查询计划及 Grok 互审记录。", "",
              "```bash", "python3 sqlite_wal_deep_validation.py --binary ./sqlite_outbox_benchmark \\",
              "  --new-library ./libsqlite3-eval.so --output ./deep-new \\",
              "  --seed-bytes 8589934592 --seconds 300", "",
              "unshare --mount --propagation private -- sh sqlite_enospc_namespace.sh /absolute/test-directory enospc-new", "```", "",
              "输出目录必须未存在，测试目录必须与生产数据隔离。", "",
              "## 尚未完成的投产门禁", "",
              "- 104 真实存量库副本、真实积压比例、混合 Full/告警/变位/清理负载及跨日长跑。",
              "- 物理掉电、eMMC 持久性、真实文件系统 ENOSPC/IOERR、真实 MQTT 故障与告警端到端时延。",
              "- 私有库加载失败不得静默回退；进程内 SQLite 函数表混库风险；跨进程统一写入口与公平调度。",
              "- 需要补齐同版本、相同注入、随机顺序的恢复对照，才能进一步分离 FULL 同步开销、调度及缓存等因素。",
              "- 未逐条记录模拟发送回调的 event_id/payload；历史核验和计数守恒不能替代回放内容逐条核验。",
              "- 本次为分钟级测试，不能外推 10 分钟锁失败门槛或长期资源上限；工作进程不包含生产服务和内核全部资源。", ""]
    status_path = args.results.parent / "status-final.json"
    if status_path.exists():
        status = json.loads(status_path.read_text(encoding="utf-8"))
        lines += ["## 业务服务复核", "", f"结束快照：{status['timestamp']}。", "",
                  "|服务|PID|状态|自动重启次数|VmRSS MiB|", "|---|---:|---|---:|---:|"]
        for name, service in status["services"].items():
            rss = int(service["memory"]["VmRSS"].split()[0]) / 1024
            lines.append(f"|{name}|{service['MainPID']}|{service['ActiveState']}|{service['NRestarts']}|{rss:.2f}|")
        lines += ["", "三项 PID 和二进制 SHA256 与本轮之前的基线相同；相关服务日志未匹配到新增 SQLite 锁、入队、回放、发布失败。"
                  "业务表为单次 VmRSS 快照，与负载表的各进程 VmHWM 峰值之和不同。"
                  "这不等于共享 eMMC 的全库扫描对业务时延完全没有影响；本轮没有测量实际业务端到端延迟。", ""]
    cleanup_path = args.results.parent / "fixture-cleanup.json"
    if cleanup_path.exists():
        removed = json.loads(cleanup_path.read_text(encoding="utf-8"))
        lines += ["## 测试空间回收", "", f"结果拉回且测试进程全部退出后，仅清理本轮 smoke/deep 的 history.db 及其 sidecar/标记文件，"
                  f"删除文件名义大小合计 {sum(r['bytes'] for r in removed) / 1024**3:.3f} GiB。原始 JSON、日志与测试工具保留。", ""]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text("\n".join(lines), encoding="utf-8")
    with args.output.with_suffix(".csv").open("w", encoding="utf-8-sig", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    args.output.with_suffix(".json").write_text(json.dumps(dict(stages=rows, enospc=enospc,
        seed={k:v for k,v in seed.items() if k != "signature"},
        valid=deep["dataValid"]), ensure_ascii=False, indent=2), encoding="utf-8")
    print(args.output)


if __name__ == "__main__":
    main()
