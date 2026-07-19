#!/bin/sh
set -eu

ROOT="${TMPDIR:-/tmp}/gateway-scada-install-test-$$"
SOURCE="$ROOT/source"
PACKAGE="$ROOT/site-a.kyscada"
APP_CONFIG="$ROOT/monitor.json"
SCADA_ROOT="$ROOT/runtime"
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

printf '%s\n' '{"localDisplay":{"enabled":true,"renderer":"nativeQt"}}' > "$APP_CONFIG"

sh "$INSTALLER" \
    --package "$PACKAGE" \
    --machine-code COMM202600999 \
    --app-config "$APP_CONFIG" \
    --scada-root "$SCADA_ROOT" \
    --dry-run

sh "$INSTALLER" \
    --package "$PACKAGE" \
    --machine-code COMM202600999 \
    --app-config "$APP_CONFIG" \
    --scada-root "$SCADA_ROOT"

[ -L "$SCADA_ROOT/current" ]
[ -f "$SCADA_ROOT/current/manifest.json" ]
python3 - "$APP_CONFIG" "$SCADA_ROOT/current" <<'PY'
import json
import sys

path, expected_directory = sys.argv[1:]
with open(path, "r", encoding="utf-8") as source:
    scada = json.load(source)["localDisplay"]["scada"]
assert scada["enabled"] is True
assert scada["nodeId"] == "edge-a"
assert scada["projectDirectory"] == expected_directory
PY

echo "scada_install_test passed"
