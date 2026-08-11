#!/bin/sh
set -eu

ROOT="${TMPDIR:-/tmp}/gateway-scada-install-test-$$"
SOURCE="$ROOT/source"
PACKAGE="$ROOT/site-a.kyscada"
APP_CONFIG="$ROOT/monitor.json"
SCADA_ROOT="$ROOT/runtime"
STATE_FILE="$ROOT/install-state.txt"
INSTALLER="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)/deploy/install-scada-project.sh"

cleanup() {
    rm -rf "$ROOT"
}
trap cleanup EXIT INT TERM

mkdir -p "$SOURCE/screens"
python3 - "$PACKAGE" <<'PY'
import hashlib
import json
import sys
import zipfile

package = sys.argv[1]
documents = {
    "manifest.json": {
        "schemaVersion": "2.0",
        "projectId": "site-a",
        "projectName": "test",
        "packageVersion": "1.0.0",
        "entryScreen": "overview",
        "packageRole": "project",
    },
    "topology.json": {
        "mode": "integrated",
        "scadaHost": "edge",
        "emsHost": "edge",
        "dataTransport": "sharedMemory",
        "offlinePolicy": "continueLocal",
    },
    "nodes.json": [
        {"nodeId": "edge-a", "machineCode": "COMM202600999", "displayName": "test"}
    ],
    "tags.json": [],
    "runtime-map.json": [],
    "screens/overview.json": {
        "screenId": "overview",
        "title": "test",
        "width": 1920,
        "height": 1080,
        "widgets": [],
    },
}
contents = {
    path: (json.dumps(value, ensure_ascii=False, indent=2) + "\n").encode("utf-8")
    for path, value in documents.items()
}
checksums = {path: hashlib.sha256(content).hexdigest() for path, content in contents.items()}
contents["checksums.json"] = (json.dumps(checksums, indent=2) + "\n").encode("utf-8")
with zipfile.ZipFile(package, "w", zipfile.ZIP_DEFLATED) as archive:
    for path, content in contents.items():
        archive.writestr(path, content)
PY

PACKAGE_SHA256="$(sha256sum "$PACKAGE" | awk '{print $1}')"

printf '%s\n' '{"localDisplay":{"enabled":false,"renderer":"webkit"}}' > "$APP_CONFIG"

if sh "$INSTALLER" \
    --package "$PACKAGE" \
    --package-sha256 0000000000000000000000000000000000000000000000000000000000000000 \
    --machine-code COMM202600999 \
    --app-config "$APP_CONFIG" \
    --scada-root "$SCADA_ROOT" \
    --require-local-scada \
    --no-restart >/dev/null 2>&1; then
    echo "SCADA installer accepted an incorrect package SHA256" >&2
    exit 1
fi
[ ! -e "$SCADA_ROOT/current" ]

sh "$INSTALLER" \
    --package "$PACKAGE" \
    --package-sha256 "$PACKAGE_SHA256" \
    --machine-code COMM202600999 \
    --app-config "$APP_CONFIG" \
    --scada-root "$SCADA_ROOT" \
    --require-local-scada \
    --no-restart \
    --dry-run

sh "$INSTALLER" \
    --package "$PACKAGE" \
    --package-sha256 "$PACKAGE_SHA256" \
    --machine-code COMM202600999 \
    --app-config "$APP_CONFIG" \
    --scada-root "$SCADA_ROOT" \
    --require-local-scada \
    --no-restart \
    --state-file "$STATE_FILE"

[ -L "$SCADA_ROOT/current" ]
[ -f "$SCADA_ROOT/current/manifest.json" ]
[ -f "$STATE_FILE" ]
python3 - "$APP_CONFIG" "$SCADA_ROOT/current" "$STATE_FILE" "$PACKAGE_SHA256" <<'PY'
import json
import sys

path, expected_directory, state_path, expected_package_sha256 = sys.argv[1:]
with open(path, "r", encoding="utf-8") as source:
    local_display = json.load(source)["localDisplay"]
scada = local_display["scada"]
assert local_display["enabled"] is True
assert local_display["renderer"] == "nativeQt"
assert scada["enabled"] is True
assert scada["nodeId"] == "edge-a"
assert scada["projectDirectory"] == expected_directory
state = dict(line.rstrip("\n").split("=", 1) for line in open(state_path, encoding="utf-8") if "=" in line)
assert state["scadaRoot"] == expected_directory.rsplit("/current", 1)[0]
assert state["scadaCurrentTarget"] == str(__import__("pathlib").Path(expected_directory).resolve())
assert state["scadaVersion"] == "1.0.0"
assert state["scadaPackageSha256"] == expected_package_sha256
PY

echo "scada_install_test passed"
