#!/bin/sh
set -eu

ROOT="${TMPDIR:-/tmp}/gateway-scada-rollback-test-$$"
APP_CONFIG="$ROOT/monitor.json"
SCADA_ROOT="$ROOT/scada"
OTA_BACKUP="$ROOT/ota/backup"
OTA_STAGING="$ROOT/ota/staging"
INSTALLER="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)/deploy/install-scada-project.sh"
ROLLBACK="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)/deploy/ota-rollback.sh"

cleanup() {
    rm -rf "$ROOT"
}
trap cleanup EXIT INT TERM

make_package() {
    package="$1"
    version="$2"
    python3 - "$package" "$version" <<'PY'
import hashlib
import json
import sys
import zipfile

package, version = sys.argv[1:]
documents = {
    "manifest.json": {"schemaVersion":"2.0","projectId":"rollback-site","projectName":"test","packageVersion":version,"entryScreen":"overview","packageRole":"project"},
    "topology.json": {"mode":"integrated","scadaHost":"edge","emsHost":"edge","dataTransport":"sharedMemory","offlinePolicy":"continueLocal"},
    "nodes.json": [{"nodeId":"edge-a","machineCode":"COMM202600999","displayName":"test"}],
    "tags.json": [],
    "runtime-map.json": [],
    "screens/overview.json": {"screenId":"overview","title":"test","width":1920,"height":1080,"widgets":[]},
}
contents = {path:(json.dumps(value, ensure_ascii=False, indent=2)+"\n").encode() for path,value in documents.items()}
contents["checksums.json"] = (json.dumps({path:hashlib.sha256(data).hexdigest() for path,data in contents.items()}, indent=2)+"\n").encode()
with zipfile.ZipFile(package, "w", zipfile.ZIP_DEFLATED) as archive:
    for path, data in contents.items(): archive.writestr(path, data)
PY
}

mkdir -p "$ROOT" "$OTA_BACKUP" "$OTA_STAGING"
printf '%s\n' '{"marker":"factory","localDisplay":{"enabled":true}}' > "$APP_CONFIG"
make_package "$ROOT/v1.kyscada" "1.0.0"
make_package "$ROOT/v2.kyscada" "2.0.0"

sh "$INSTALLER" --package "$ROOT/v1.kyscada" --machine-code COMM202600999 \
    --app-config "$APP_CONFIG" --scada-root "$SCADA_ROOT" --state-file "$ROOT/v1-state.txt"

python3 - "$APP_CONFIG" <<'PY'
import json,sys
path=sys.argv[1]
data=json.load(open(path, encoding="utf-8"))
data["marker"]="before-v2"
json.dump(data, open(path,"w",encoding="utf-8"), ensure_ascii=False, indent=2)
PY

JOB_ID="SCADA_ROLLBACK_TEST"
WORK_DIR="$OTA_STAGING/$JOB_ID"
mkdir -p "$WORK_DIR"
printf '%s\n' 'packageType=scada' > "$WORK_DIR/package-type.txt"
sh "$INSTALLER" --package "$ROOT/v2.kyscada" --machine-code COMM202600999 \
    --app-config "$APP_CONFIG" --scada-root "$SCADA_ROOT" --state-file "$WORK_DIR/scada-install-state.txt"
sed -i 's/^scadaService=.*/scadaService=/' "$WORK_DIR/scada-install-state.txt"

python3 - "$APP_CONFIG" <<'PY'
import json,sys
path=sys.argv[1]
data=json.load(open(path, encoding="utf-8"))
data["marker"]="after-v2"
json.dump(data, open(path,"w",encoding="utf-8"), ensure_ascii=False, indent=2)
PY

{
    echo "jobId=$JOB_ID"
    echo "version=2.0.0"
    echo "packageType=scada"
    echo "workDir=$WORK_DIR"
    cat "$WORK_DIR/scada-install-state.txt"
} > "$OTA_STAGING/current_version.txt"

sh "$ROLLBACK" "$ROOT/v2.kyscada" "2.0.0" "$JOB_ID" "$OTA_BACKUP" "$OTA_STAGING"

python3 - "$SCADA_ROOT/current/manifest.json" "$APP_CONFIG" "$OTA_STAGING/current_version.txt" <<'PY'
import json,sys
manifest, app, state = sys.argv[1:]
assert json.load(open(manifest, encoding="utf-8"))["packageVersion"] == "1.0.0"
assert json.load(open(app, encoding="utf-8"))["marker"] == "before-v2"
values=dict(line.rstrip("\n").split("=",1) for line in open(state,encoding="utf-8") if "=" in line)
assert values["version"] == "1.0.0"
assert values["packageType"] == "scada"
PY

FAILED_JOB="SCADA_FAILED_INSTALL"
mkdir -p "$OTA_STAGING/$FAILED_JOB"
printf '%s\n' 'packageType=scada' > "$OTA_STAGING/$FAILED_JOB/package-type.txt"
before="$(readlink -f "$SCADA_ROOT/current")"
sh "$ROLLBACK" "$ROOT/v2.kyscada" "3.0.0" "$FAILED_JOB" "$OTA_BACKUP" "$OTA_STAGING"
after="$(readlink -f "$SCADA_ROOT/current")"
[ "$before" = "$after" ]

echo "scada_rollback_test passed"
