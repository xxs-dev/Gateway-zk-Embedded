#!/bin/sh
set -eu

SOURCE_DIR=${1:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}
BUILD_DIR=${2:-$SOURCE_DIR/build-wsl-native}
ROOT=$(mktemp -d /tmp/gateway-test-lab-performance-e2e.XXXXXX)
LAB_SCRIPT=$SOURCE_DIR/test-lab/scripts/gateway-test-lab.sh
PERFORMANCE_SCRIPT=$SOURCE_DIR/test-lab/scripts/gateway-test-lab-performance.sh
REPORT_DIR=$ROOT/reports

run_lab() {
    GATEWAY_TEST_LAB_ROOT="$ROOT" \
    GATEWAY_BIN_DIR="$BUILD_DIR" \
    GATEWAY_TEST_LAB_SIM_BIN="$ROOT/bin/gateway-test-lab-sim" \
    GATEWAY_TEST_LAB_PERFORMANCE_SCRIPT="$PERFORMANCE_SCRIPT" \
        "$LAB_SCRIPT" "$@"
}

cleanup() {
    result=$?
    trap - EXIT INT TERM
    run_lab stop >/dev/null 2>&1 || true
    if [ "$result" -ne 0 ] || [ "${KEEP_GATEWAY_TEST_LAB_PERFORMANCE_E2E:-0}" = "1" ]; then
        echo "performance e2e files retained at $ROOT" >&2
    else
        case "$ROOT" in /tmp/gateway-test-lab-performance-e2e.*) rm -rf -- "$ROOT" ;; esac
    fi
    exit "$result"
}
trap cleanup EXIT INT TERM

mkdir -p "$ROOT/bin"
cp "$BUILD_DIR/gateway-test-lab-sim" "$ROOT/bin/"
cp -R "$SOURCE_DIR/test-lab/templates" "$ROOT/"
chmod +x "$LAB_SCRIPT" "$PERFORMANCE_SCRIPT"

index=0
for protocol in modbus-tcp modbus-rtu dlt645 dio iec104 can; do
    index=$((index + 1))
    protocol_report=$REPORT_DIR/$protocol
    run_lab performance \
        --protocol "$protocol" \
        --port $((26020 + index)) \
        --monitor-port $((29540 + index)) \
        --can-driver-udp-port $((29110 + index * 2)) \
        --can-simulator-udp-port $((29111 + index * 2)) \
        --warmup-sec 1 \
        --duration-sec 3 \
        --sample-interval-sec 1 \
        --output-dir "$protocol_report" \
        > "$ROOT/performance-$protocol.out" \
        2> "$ROOT/performance-$protocol.err"

    [ -s "$protocol_report/report.json" ]
    [ -s "$protocol_report/report.md" ]
    if command -v python3 >/dev/null 2>&1; then
        python3 -m json.tool "$protocol_report/report.json" >/dev/null
    fi
    grep -q '"passed": true' "$protocol_report/report.json"
    grep -q "\"protocol\":\"$protocol\"" "$protocol_report/report.json"
    grep -q '"freshnessMs"' "$protocol_report/report.json"
    grep -q '"thresholds"' "$protocol_report/report.json"
    grep -q '"timing"' "$protocol_report/report.json"
    grep -q '"estimatedMinimumCycleMs"' "$protocol_report/report.json"
    grep -q '"linkUtilizationPercent"' "$protocol_report/report.json"
    grep -q '"physicalMediaTestMode":false' "$protocol_report/report.json"
    grep -q '"sourceToSharedMemoryMs"' "$protocol_report/report.json"
    grep -q '"sourceToMqttPublisherMs"' "$protocol_report/report.json"
    grep -q '"evaluatedRules"' "$protocol_report/report.json"
    grep -q '"fullUploadAchievementPercent"' "$protocol_report/report.json"
    grep -q '"normalizedCpuPercent"' "$protocol_report/report.json"
    grep -q '# 边端一键性能测试报告' "$protocol_report/report.md"
    grep -q '## 证据与限制' "$protocol_report/report.md"
    grep -q '## 协议与介质时间估算' "$protocol_report/report.md"
    [ -s "$protocol_report/raw/SHA256SUMS.txt" ]
    [ -s "$protocol_report/raw/timing-estimate.json" ]
    [ -s "$protocol_report/raw/point-snapshot-final.txt" ]
    [ -s "$protocol_report/raw/compute-correctness-attempts.tsv" ]
    [ -f "$protocol_report/raw/trace-shared-memory.tsv" ]
    [ -f "$protocol_report/raw/trace-mqtt-publisher.tsv" ]
    ! grep -q 'not found' "$ROOT/performance-$protocol.err"

    case "$protocol" in
        modbus-tcp|modbus-rtu)
            [ -s "$protocol_report/raw/trace-shared-memory.tsv" ]
            [ -s "$protocol_report/raw/trace-mqtt-publisher.tsv" ]
            grep -q '"sequenceEvidenceAvailable":true' "$protocol_report/report.json"
            grep -q '"evidenceScope":"simulator-to-mqtt-publisher"' "$protocol_report/report.json"
            ;;
        *)
            grep -q '"sequenceEvidenceAvailable":false' "$protocol_report/report.json"
            grep -q '"evidenceScope":"observation-only"' "$protocol_report/report.json"
            ;;
    esac
done

echo "gateway_test_lab_performance_report_e2e passed"
