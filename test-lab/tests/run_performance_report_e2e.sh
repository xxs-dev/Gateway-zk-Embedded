#!/bin/sh
set -eu

SOURCE_DIR=${1:-$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}
BUILD_DIR=${2:-$SOURCE_DIR/build-wsl-native}
ROOT=$(mktemp -d /tmp/gateway-test-lab-performance-e2e.XXXXXX)
LAB_SCRIPT=$SOURCE_DIR/test-lab/scripts/gateway-test-lab.sh
PERFORMANCE_SCRIPT=$SOURCE_DIR/test-lab/scripts/gateway-test-lab-performance.sh
REPORT_DIR=$ROOT/report

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

run_lab performance \
    --protocol modbus-tcp \
    --warmup-sec 2 \
    --duration-sec 5 \
    --sample-interval-sec 1 \
    --output-dir "$REPORT_DIR" \
    > "$ROOT/performance-output.txt"

[ -s "$REPORT_DIR/report.json" ]
[ -s "$REPORT_DIR/report.md" ]
if command -v python3 >/dev/null 2>&1; then
    python3 -m json.tool "$REPORT_DIR/report.json" >/dev/null
fi
grep -q '"passed": true' "$REPORT_DIR/report.json"
grep -q '"freshnessMs"' "$REPORT_DIR/report.json"
grep -q '"thresholds"' "$REPORT_DIR/report.json"
grep -q '"evaluatedRules"' "$REPORT_DIR/report.json"
grep -q '"fullUploadAchievementPercent"' "$REPORT_DIR/report.json"
grep -q '"normalizedCpuPercent"' "$REPORT_DIR/report.json"
grep -q '# 边端一键性能测试报告' "$REPORT_DIR/report.md"
grep -q '## 证据与限制' "$REPORT_DIR/report.md"
[ -s "$REPORT_DIR/raw/SHA256SUMS.txt" ]
[ -s "$REPORT_DIR/raw/point-snapshot-final.txt" ]
[ -s "$REPORT_DIR/raw/compute-correctness-attempts.tsv" ]
grep -q '"passed":true' "$REPORT_DIR/report.json"

echo "gateway_test_lab_performance_report_e2e passed"
