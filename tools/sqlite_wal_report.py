#!/usr/bin/env python3
"""Render the isolated SQLite evaluation JSON as Chinese Markdown and CSV."""
import argparse
import csv
import json
from pathlib import Path


def percentile(values, fraction):
    values = sorted(values)
    return values[min(len(values) - 1, int((len(values) - 1) * fraction))] if values else 0


def compact(result):
    workers = result["workers"]
    producers = [x for x in workers if x["role"] == "producer"]
    replay = next(x for x in workers if x["role"] == "replay")
    reader = next(x for x in workers if x["role"] == "reader")
    elapsed = max(x["elapsedMs"] for x in workers) / 1000
    latency = [v for x in producers for v in x["latencyMs"]]
    failures = [v for x in workers for v in x["errorLatencyMs"]]
    committed_sent = int(result["beforeDrain"]["rows"]) - int(result["beforeDrain"]["pending"])
    return dict(name=result["name"], load=result["load"], repeat=result["repeat"],
                version=workers[0]["version"], synchronous=workers[0]["synchronous"],
                accepted=sum(x["events"] for x in producers),
                ingressPerSec=sum(x["events"] for x in producers) / elapsed,
                replayPerSec=committed_sent / elapsed,
                replayApiReturned=replay["events"], replayCommitted=committed_sent,
                sendCallbacks=replay["sends"], extraSendCallbacks=replay["sends"] - committed_sent,
                p50Ms=percentile(latency, .5), p95Ms=percentile(latency, .95),
                p99Ms=percentile(latency, .99), maxMs=max(latency, default=0),
                errorCount=sum(x["errors"] for x in workers), maxErrorMs=max(failures, default=0),
                readerP99Ms=reader["p99Ms"], cpuOneCorePercent=sum(x["cpuSeconds"] for x in workers) / elapsed * 100,
                rssSumMiB=(sum(x["rssMaxKiB"] for x in workers) / 1024
                           if all(x.get("rssMeasurement") == "proc-status-VmHWM" for x in workers) else None),
                writeMiB=(sum(x["writeBytes"] for x in workers) / 1024**2
                          if all(x.get("ioAccountingAvailable", False) for x in workers) else None),
                peakWalMiB=result["peakWalBytes"] / 1024**2,
                pendingAfterLoad=int(result["beforeDrain"]["pending"]),
                drainSeconds=result["drain"]["elapsedMs"] / 1000,
                integrity=result["final"]["integrity"], dataConsistent=(result["final"]["valid"]
                    and int(result["final"]["rows"]) == sum(x["events"] for x in producers)
                    and sum(x["events"] for x in producers) == committed_sent + result["drain"]["events"]
                    and int(result["final"]["states"]) == 32),
                pendingAfterDrain=int(result["final"]["pending"]), strictPass=result["valid"])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("summary", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    data = json.loads(args.summary.read_text(encoding="utf-8"))
    rows = [compact(x) for x in data["results"]]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.with_suffix(".csv").open("w", newline="", encoding="utf-8-sig") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    args.output.with_suffix(".json").write_text(json.dumps(dict(runs=rows, probes=data["probes"]),
                                                         indent=2, ensure_ascii=False), encoding="utf-8")
    lines = ["# 22.16 SQLite 新版与 WAL 隔离评估报告", "", "日期：2026-09-07。设备：COMM202600999。",
             "", "## 范围与口径", "",
             "- 设备为全志 AArch64、约 4GB 内存，eMMC 根分区 ext4，测试前可用约 18GB。",
             "- 系统库 SQLite 3.31.1；候选私有库 SQLite 3.53.4，由 22.11 的 GCC 6.3.1 交叉编译。",
             "- 不替换系统库，不改业务配置，不发布 MQTT，不操作 PCS，不重启业务服务或设备。",
             "- 每轮使用新的独立数据库；直接编译当前 mqtt_event_outbox.cpp，保留现有入队、状态入库、领取、发送、ACK、重试逻辑。",
             "- 两个入队进程，每批 16 条、每条 1KiB、同时更新 16 个状态；一个回放进程和一个短读进程。",
             "- 固定负载每个生产者每 100ms 一批，合计目标 320 条/秒；饱和负载不等待。过期批次不追赶，因此目标值不等于实际完成吞吐。",
             "- 回放每次最多 64 条，每发送子批最多 8 条，模拟等待 2ms；网络等待保持在事务外。读进程每 5ms 查询增量 pending 统计。",
             "- 五组模式、两类负载、各三轮 10 秒，固定随机种子打乱顺序；另做新版 DELETE/NORMAL 和 WAL/FULL 各 120 秒。",
             "- 延迟为每次整批入队 API 的端到端耗时，含内部重试；百分位仅统计成功调用，失败次数和失败最长耗时另外列出。不是单条事件延迟。",
             "- CPU 100% 表示占满一个核心；内存仅采纳补测的 /proc/PID/status VmHWM，舍弃初版 getrusage 的启动继承峰值。RSS 为各进程峰值之和，不是同时峰值。",
             "- 板厂内核未提供 /proc/PID/io，写入量不可测。初版原始 JSON 的 writeBytes=0/readBytes=0 是缺失值，不是零 I/O；修订工具输出可用标志及 -1。未给出 eMMC 写放大结论。",
             "- WAL 峰值每 20ms 采样，可能错过极短峰值。饱和入队吞吐不等于完整上传吞吐，必须一起看回放速率与积压。",
             "- dataConsistent 与 strictPass 分开：锁等待耗尽可以使 strictPass=false，但不表示已提交数据损坏。",
             "- 回放调用可能先提交部分 ACK，随后抛错而不返回前缀统计。回放速率按数据库已提交 sent 行计算，不使用可能少计的函数返回值；CSV 保留两种数值及模拟发送回调次数。",
             "", "## 三轮对照汇总", "",
             "下表延迟为三轮全部成功入队调用合并后的百分位；速率、CPU 取各轮平均；WAL 取最大采样值。",
             "", "|模式|负载|入队条/秒|回放条/秒|入队 P95/P99(ms)|失败数|CPU单核%|写入MiB/轮|WAL峰值MiB|",
             "|---|---|---:|---:|---:|---:|---:|---:|---:|"]
    names = ["old-delete-normal", "new-delete-normal", "new-delete-full", "new-wal-normal", "new-wal-full"]
    for load in ("fixed", "saturation"):
        for name in names:
            group = [r for r in rows if r["name"] == name and r["load"] == load and r["repeat"] != "soak"]
            source = [r for r in data["results"] if r["name"] == name and r["load"] == load and r["repeat"] != "soak"]
            latency = [v for r in source for w in r["workers"] if w["role"] == "producer" for v in w["latencyMs"]]
            avg = lambda key: sum(r[key] for r in group) / len(group)
            lines.append(f"|{name}|{'固定' if load == 'fixed' else '饱和'}|{avg('ingressPerSec'):.1f}|"
                         f"{avg('replayPerSec'):.1f}|{percentile(latency, .95):.2f}/{percentile(latency, .99):.2f}|"
                         f"{sum(r['errorCount'] for r in group)}|{avg('cpuOneCorePercent'):.1f}|"
                         f"不可测|{max(r['peakWalMiB'] for r in group):.1f}|")
    lines += ["", "## 持续运行与完整性", "", "|模式|时长|入队条/秒|入队P95/P99(ms)|查询P99(ms)|CPU单核%|失败数|结束积压|WAL峰值MiB|",
              "|---|---:|---:|---:|---:|---:|---:|---:|---:|"]
    for r in rows:
        if r["repeat"] == "soak":
            lines.append(f"|{r['name']}|120秒|{r['ingressPerSec']:.1f}|{r['p95Ms']:.2f}/{r['p99Ms']:.2f}|"
                         f"{r['readerP99Ms']:.2f}|{r['cpuOneCorePercent']:.2f}|{r['errorCount']}|"
                         f"{r['pendingAfterLoad']}|{r['peakWalMiB']:.1f}|")
    lines += ["", f"- 正式运行共 {len(rows)} 轮；完整性及统计一致性通过 {sum(r['dataConsistent'] and r['integrity'] == 'ok' for r in rows)}/{len(rows)}。",
              f"- 所有正式轮次排空后 pending=0：{all(r['pendingAfterDrain'] == 0 for r in rows)}。",
              "- 校验内容：integrity_check、事件总数与成功返回的 ID 数、event_id 唯一性、待发条数/字节统计、32 个最新状态行、回放计数、最终积压。",
              "- 原始 JSON 保留每次成功/失败调用延迟、每进程指标、业务连接实际 journal/synchronous/busy_timeout/wal_autocheckpoint、排空前后校验结果。",
              "", "## 专项故障探针", "",
              "|模式|读事务期间写提交 rc|第二写者 rc|读后写扩展码|模拟库容量满 rc|强杀后已提交行|通过|",
              "|---|---:|---:|---:|---:|---|---|"]
    for name, p in data["probes"].items():
        lines.append(f"|{name}|{p['readerCommit']['rc']}|{p['writerBusy']['rc']}|"
                     f"{p.get('snapshotUpgrade', {}).get('extended', '-')}|{p['full']['rc']}|"
                     f"{p['crashRows']}|{p['valid']}|")
    lines += ["", "- rc=5 为 SQLITE_BUSY；扩展码 517 为 SQLITE_BUSY_SNAPSHOT；rc=13 为 SQLITE_FULL。本次没有构造 shared-cache 的 SQLITE_LOCKED。",
              "- WAL 长读事务探针持有 BEGIN+SELECT 快照，观察 checkpoint 无法追上；释放读事务后 TRUNCATE 成功回收 WAL。",
              "- 强杀探针确认两条已提交记录保留、一条未提交记录回滚，完整性通过；仅证明进程崩溃恢复，不证明突然断电不会丢数据。",
              "- 容量满通过 max_page_count 构造，不填满设备磁盘；验证错误返回、回滚后可继续提交及完整性。不是实际 ENOSPC/IOERR 测试。",
              "", "## 尚未覆盖", "",
              "1. 104 的约 8.4GB 存量库迁移耗时、长周期清理、连续多日运行及真实业务负载。",
              "2. 物理掉电、eMMC 控制器缓存持久性、文件系统故障、真实磁盘耗尽。",
              "3. MQTT 网络抖动、断线、实际 PUBACK 以及多平台发送压力；本测试不接真实 broker。",
              "   负载为告警/变位事件，不代表生产 Full 大报文、历史清理、VACUUM 的混合负载。",
              "4. Outbox 内部每次 SQLite 重试的扩展码计数、BEGIN 到 COMMIT 的单独计时、入队到最终 ACK 的逐条延迟。",
              "5. 新旧库编译选项完全对齐后的纯版本比较。这里比较系统旧库与候选私有构建组合，不把全部差异归因于版本号。",
              "6. 120 秒只用于短期稳定性筛查，不能据此承诺长期 WAL 上限和 eMMC 寿命。",
              "", "## 复测方式", "", "在测试机的独立目录运行，output 必须为尚不存在的目录：", "", "```bash",
              "python3 sqlite_wal_evaluation.py --binary ./sqlite_outbox_benchmark \\",
              "  --new-library ./libsqlite3-eval.so --output ./results-new \\",
              "  --seconds 10 --repeats 3 --soak-seconds 120", "```", "",
              "将 summary.json 拉回后生成中文报告和 CSV：", "", "```bash",
              "python tools/sqlite_wal_report.py results/summary.json report.md", "```", ""]
    args.output.write_text("\n".join(lines), encoding="utf-8")
    print(args.output)


if __name__ == "__main__":
    main()
