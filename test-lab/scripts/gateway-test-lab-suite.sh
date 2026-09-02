#!/bin/sh
set -eu

LC_ALL=C
export LC_ALL

ROOT=${GATEWAY_TEST_LAB_ROOT:-/opt/modbus-gateway/test-lab}
LAB_COMMAND=${GATEWAY_TEST_LAB_COMMAND:-$ROOT/bin/gateway-test-lab}

protocols=modbus-tcp,modbus-rtu,dlt645,dio,iec104,can
scenario=normal
mode=virtual
warmup_sec=60
duration_sec=600
sample_interval_sec=1
output_dir=
mqtt_broker=
base_port=26020
base_monitor_port=29540
base_can_port=29110

usage() {
    cat <<'EOF'
Usage: gateway-test-lab suite [options]
  --protocols LIST              Comma-separated protocols; defaults to all supported protocols
  --scenario NAME               normal|ramp|random|boundary
  --mode MODE                   virtual|hil
  --warmup-sec N                Warm-up duration for each protocol
  --duration-sec N              Measurement duration for each protocol
  --sample-interval-sec N       Resource sample interval
  --output-dir DIR              Suite report directory
  --mqtt-broker URL             Optional test MQTT broker
  --base-port N                 Base simulator TCP port
  --base-monitor-port N         Base SystemMonitor HTTP port
  --base-can-port N             Base UDP port for virtual CAN
EOF
}

require_uint() {
    name=$1
    value=$2
    case "$value" in ''|*[!0-9]*) echo "$name must be a non-negative integer" >&2; exit 2 ;; esac
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --protocols) protocols=$2; shift 2 ;;
        --scenario) scenario=$2; shift 2 ;;
        --mode) mode=$2; shift 2 ;;
        --warmup-sec) warmup_sec=$2; shift 2 ;;
        --duration-sec) duration_sec=$2; shift 2 ;;
        --sample-interval-sec) sample_interval_sec=$2; shift 2 ;;
        --output-dir) output_dir=$2; shift 2 ;;
        --mqtt-broker) mqtt_broker=$2; shift 2 ;;
        --base-port) base_port=$2; shift 2 ;;
        --base-monitor-port) base_monitor_port=$2; shift 2 ;;
        --base-can-port) base_can_port=$2; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown suite option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

case "$scenario" in normal|ramp|random|boundary) ;; *) echo "invalid scenario: $scenario" >&2; exit 2 ;; esac
case "$mode" in virtual|hil) ;; *) echo "invalid mode: $mode" >&2; exit 2 ;; esac
require_uint warmup-sec "$warmup_sec"
require_uint duration-sec "$duration_sec"
require_uint sample-interval-sec "$sample_interval_sec"
require_uint base-port "$base_port"
require_uint base-monitor-port "$base_monitor_port"
require_uint base-can-port "$base_can_port"
[ "$duration_sec" -gt 0 ] || { echo "duration-sec must be greater than zero" >&2; exit 2; }
[ "$sample_interval_sec" -gt 0 ] || { echo "sample-interval-sec must be greater than zero" >&2; exit 2; }
[ -x "$LAB_COMMAND" ] || { echo "gateway test-lab command not found: $LAB_COMMAND" >&2; exit 2; }
command -v python3 >/dev/null 2>&1 || { echo "python3 is required to build the suite report" >&2; exit 2; }

if [ -z "$output_dir" ]; then
    output_dir=$ROOT/reports/suite-$(date +%Y%m%d-%H%M%S)
fi
mkdir -p "$output_dir/logs"
results_file=$output_dir/protocol-results.tsv
: > "$results_file"

protocol_list=$(printf '%s' "$protocols" | tr ',' ' ')
[ -n "$protocol_list" ] || { echo "protocols must not be empty" >&2; exit 2; }

index=0
failed=0
for protocol in $protocol_list; do
    case "$protocol" in
        modbus-tcp|modbus-rtu|dlt645|dio|iec104|can) ;;
        *) echo "unsupported protocol in suite: $protocol" >&2; exit 2 ;;
    esac
    index=$((index + 1))
    protocol_report=$output_dir/$protocol

    echo "[$index] testing $protocol ..."
    if [ -n "$mqtt_broker" ]; then
        if "$LAB_COMMAND" performance \
                --protocol "$protocol" --scenario "$scenario" --mode "$mode" \
                --port $((base_port + index)) --monitor-port $((base_monitor_port + index)) \
                --can-driver-udp-port $((base_can_port + index * 2)) \
                --can-simulator-udp-port $((base_can_port + index * 2 + 1)) \
                --warmup-sec "$warmup_sec" --duration-sec "$duration_sec" \
                --sample-interval-sec "$sample_interval_sec" --output-dir "$protocol_report" \
                --mqtt-broker "$mqtt_broker" \
                > "$output_dir/logs/$protocol.out" 2> "$output_dir/logs/$protocol.err"; then
            result=PASS
            exit_code=0
        else
            exit_code=$?
            result=FAIL
            failed=$((failed + 1))
        fi
    elif "$LAB_COMMAND" performance \
            --protocol "$protocol" --scenario "$scenario" --mode "$mode" \
            --port $((base_port + index)) --monitor-port $((base_monitor_port + index)) \
            --can-driver-udp-port $((base_can_port + index * 2)) \
            --can-simulator-udp-port $((base_can_port + index * 2 + 1)) \
            --warmup-sec "$warmup_sec" --duration-sec "$duration_sec" \
            --sample-interval-sec "$sample_interval_sec" --output-dir "$protocol_report" \
            > "$output_dir/logs/$protocol.out" 2> "$output_dir/logs/$protocol.err"; then
        result=PASS
        exit_code=0
    else
        exit_code=$?
        result=FAIL
        failed=$((failed + 1))
    fi
    printf '%s\t%s\t%s\n' "$protocol" "$exit_code" "$result" >> "$results_file"
    echo "[$index] $protocol $result"
done

python3 - "$output_dir" "$results_file" "$scenario" "$mode" \
        "$warmup_sec" "$duration_sec" "$sample_interval_sec" <<'PY'
import datetime
import json
import pathlib
import sys

output_dir = pathlib.Path(sys.argv[1])
results_path = pathlib.Path(sys.argv[2])
scenario, mode = sys.argv[3], sys.argv[4]
warmup_sec, duration_sec, sample_interval_sec = map(int, sys.argv[5:8])
rows = []
for line in results_path.read_text(encoding="utf-8").splitlines():
    protocol, exit_code, result = line.split("\t")
    report_path = output_dir / protocol / "report.json"
    report = None
    if report_path.is_file():
        try:
            report = json.loads(report_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            report = None
    timing = {} if report is None else report.get("timing") or {}
    rows.append({
        "protocol": protocol,
        "passed": result == "PASS" and bool(report and report.get("passed") is True),
        "exitCode": int(exit_code),
        "reportJson": f"{protocol}/report.json",
        "reportMarkdown": f"{protocol}/report.md",
        "result": None if report is None else report.get("result"),
        "timingVerdict": timing.get("verdict"),
        "configuredCycleMs": timing.get("configuredCycleMs"),
        "estimatedMinimumCycleMs": timing.get("estimatedMinimumCycleMs"),
        "collection": None if report is None else report.get("collection"),
        "compute": None if report is None else report.get("compute"),
        "mqtt": None if report is None else report.get("mqtt"),
        "resources": None if report is None else report.get("resources"),
        "endToEnd": None if report is None else report.get("endToEnd"),
        "checks": None if report is None else report.get("checks"),
    })

passed_count = sum(1 for row in rows if row["passed"])
timing_warning_count = sum(1 for row in rows if row["timingVerdict"] not in (None, "pass"))
summary = {
    "schemaVersion": "1.0",
    "testType": "gateway-multi-protocol-suite",
    "generatedAt": datetime.datetime.now(datetime.timezone.utc).isoformat(),
    "passed": passed_count == len(rows),
    "test": {
        "scenario": scenario,
        "mode": mode,
        "warmupSecPerProtocol": warmup_sec,
        "durationSecPerProtocol": duration_sec,
        "sampleIntervalSec": sample_interval_sec,
    },
    "totals": {
        "protocols": len(rows),
        "passed": passed_count,
        "failed": len(rows) - passed_count,
        "timingWarnings": timing_warning_count,
    },
    "protocols": rows,
}
(output_dir / "suite-report.json").write_text(
    json.dumps(summary, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
)

lines = [
    "# 边端全协议一键测试报告",
    "",
    "## 结论",
    "",
    "**PASS**" if summary["passed"] else "**FAIL**",
    "",
    f"协议总数：{len(rows)}，通过：{passed_count}，失败：{len(rows) - passed_count}。",
    "",
    "## 协议结果",
    "",
    "| 协议 | 结果 | 时序估算 | 配置周期 | 理论最小周期 | 详细报告 |",
    "| --- | --- | --- | ---: | ---: | --- |",
]
for row in rows:
    state = "通过" if row["passed"] else "失败"
    timing_state = {"pass": "通过", "fail": "警告"}.get(row["timingVerdict"], "无")
    configured = "-" if row["configuredCycleMs"] is None else f"{row['configuredCycleMs']} ms"
    minimum = "-" if row["estimatedMinimumCycleMs"] is None else f"{row['estimatedMinimumCycleMs']} ms"
    lines.append(
        f"| {row['protocol']} | {state} | {timing_state} | {configured} | {minimum} | [{row['protocol']}/report.md]({row['protocol']}/report.md) |"
    )
lines.extend([
    "",
    "## 测试口径",
    "",
    f"- 场景：`{scenario}`；模式：`{mode}`。",
    f"- 每种协议预热 `{warmup_sec}s`，正式采样 `{duration_sec}s`，资源采样间隔 `{sample_interval_sec}s`。",
    "- 每种协议使用独立端口、共享内存、日志和报告目录；单项失败不会阻止后续协议执行。",
    "- Modbus TCP/RTU 提供模拟源到 MQTT 发布调用的序列证据；其他协议按点位观察新鲜度给出证据范围。",
    "- 时序估算警告不等同于本次功能失败，但表示配置周期低于按帧数和介质参数估算的完整轮询周期，投产前必须调整或用 HIL 实测确认。",
    "",
])
(output_dir / "suite-report.md").write_text("\n".join(lines), encoding="utf-8")
PY

printf '\nsuite-report.json: %s\nsuite-report.md:   %s\n\n' \
    "$output_dir/suite-report.json" "$output_dir/suite-report.md"
cat "$output_dir/suite-report.md"

[ "$failed" -eq 0 ]
