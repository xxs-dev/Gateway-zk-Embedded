#!/bin/sh
set -eu

GATEWAY_HOME="${GATEWAY_HOME:-/opt/modbus-gateway}"
MODE="${1:-strict}"
MANIFEST="${EDGE_PACKAGE_MANIFEST:-$GATEWAY_HOME/config/runtime/edge-package-manifest.json}"
CURRENT="$GATEWAY_HOME/scada/current"
RELEASES_ROOT="$GATEWAY_HOME/scada/releases"
INSTALL_STATE="$GATEWAY_HOME/scada/factory-install-state.txt"
OUTPUT="$GATEWAY_HOME/scada/factory-readiness.json"
VERIFIER="$GATEWAY_HOME/bin/verify-scada-active-project.py"

case "$MODE" in
  strict|strict-required|allow-pending) ;;
  *) echo "usage: gateway-scada-readiness.sh [strict|strict-required|allow-pending]" >&2; exit 2 ;;
esac

[ -f "$MANIFEST" ] || exit 0
[ -f "$VERIFIER" ] || {
  echo "SCADA active-project verifier is missing: $VERIFIER" >&2
  exit 1
}
command -v python3 >/dev/null 2>&1 || {
  echo "python3 is required for SCADA active-project verification" >&2
  exit 1
}

if [ "$MODE" = "allow-pending" ]; then
  exec python3 "$VERIFIER" \
    --manifest "$MANIFEST" \
    --current "$CURRENT" \
    --releases-root "$RELEASES_ROOT" \
    --install-state "$INSTALL_STATE" \
    --output "$OUTPUT" \
    --allow-missing-current \
    --require-scada-contract
fi

set -- python3 "$VERIFIER" \
  --manifest "$MANIFEST" \
  --current "$CURRENT" \
  --releases-root "$RELEASES_ROOT" \
  --install-state "$INSTALL_STATE" \
  --output "$OUTPUT"
if [ "$MODE" = "strict-required" ]; then
  set -- "$@" --require-scada-contract
fi
exec "$@"
