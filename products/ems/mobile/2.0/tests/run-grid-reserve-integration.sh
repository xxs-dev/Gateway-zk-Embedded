#!/usr/bin/env bash
# Run from the repository root, in WSL/Linux. No network or physical driver.
set -euo pipefail
root=$(cd "$(dirname "$0")/../../../../.." && pwd)
cd "$root"
: "${COMM104_RUNTIME:?Set COMM104_RUNTIME to the offline runtime directory}"
: "${COMM104_GRAPH:?Set COMM104_GRAPH to the audited historical V2 JSON}"
library=${EDGE_GATEWAY_LIBRARY:-build-wsl-rootfix/libedge_gateway.a}
result=$(mktemp -d /tmp/comm104-reserve-test.XXXXXX)
python3 -B products/ems/mobile/2.0/tools/prepare-grid-reserve.py \
    --runtime "$COMM104_RUNTIME" --graph "$COMM104_GRAPH" \
    --output "$result/candidate" --max-phase-kw 41.6 --initial-mode "${COMM104_INITIAL_MODE:-0}"
python3 -B -m unittest discover -s products/ems/mobile/2.0/tests -p test_prepare_grid_reserve.py
g++ -std=c++17 -g -Iinclude products/ems/mobile/2.0/tests/grid_reserve_integration_test.cpp \
    "$library" -ldl -lpthread -lrt -o "$result/grid_reserve_integration_test"
"$result/grid_reserve_integration_test" "$result/candidate/grid-reserve.graph.json"
printf 'Offline test artifacts: %s\n' "$result"
