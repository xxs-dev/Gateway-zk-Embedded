#!/bin/sh
set -eu

PACKAGE=""
PACKAGE_SHA256=""
APP_CONFIG="/opt/modbus-gateway/config/runtime/apps/monitor-service.json"
MACHINE_CODE=""
SCADA_ROOT="/opt/modbus-gateway/scada"
DRY_RUN=0
RESTART=0
REQUIRE_LOCAL_SCADA=0
STATE_FILE=""

usage() {
    cat <<'EOF'
Usage: install-scada-project.sh --package FILE --machine-code CODE [options]
  --package-sha256 SHA256
                      require the whole .kyscada file to match this SHA256
  --app-config FILE   monitor-service.json path
  --scada-root DIR    release root; default /opt/modbus-gateway/scada
  --dry-run           validate only
  --restart           restart only the configured local SCADA display service
  --no-restart        explicitly keep services stopped; this is the default
  --require-local-scada
                      reject edge-node packages that do not provide local SCADA
  --state-file FILE   write rollback metadata after successful activation
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --package) PACKAGE="${2:-}"; shift 2 ;;
        --package-sha256) PACKAGE_SHA256="${2:-}"; shift 2 ;;
        --machine-code) MACHINE_CODE="${2:-}"; shift 2 ;;
        --app-config) APP_CONFIG="${2:-}"; shift 2 ;;
        --scada-root) SCADA_ROOT="${2:-}"; shift 2 ;;
        --dry-run) DRY_RUN=1; shift ;;
        --restart) RESTART=1; shift ;;
        --no-restart) RESTART=0; shift ;;
        --require-local-scada) REQUIRE_LOCAL_SCADA=1; shift ;;
        --state-file) STATE_FILE="${2:-}"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

[ -n "$PACKAGE" ] || { echo "--package is required" >&2; exit 2; }
[ -n "$MACHINE_CODE" ] || { echo "--machine-code is required" >&2; exit 2; }
[ -f "$PACKAGE" ] || { echo "SCADA package not found: $PACKAGE" >&2; exit 2; }
[ -f "$APP_CONFIG" ] || { echo "app config not found: $APP_CONFIG" >&2; exit 2; }
case "$SCADA_ROOT" in
    /*) ;;
    *) echo "--scada-root must be an absolute path" >&2; exit 2 ;;
esac
[ "$SCADA_ROOT" != "/" ] || { echo "--scada-root may not be /" >&2; exit 2; }
if [ -n "$STATE_FILE" ]; then
    case "$STATE_FILE" in
        /*) ;;
        *) echo "--state-file must be an absolute path" >&2; exit 2 ;;
    esac
    [ "$STATE_FILE" != "/" ] || { echo "--state-file may not be /" >&2; exit 2; }
fi
command -v python3 >/dev/null 2>&1 || { echo "python3 is required" >&2; exit 2; }

META_FILE="$(mktemp /tmp/gateway-scada-meta.XXXXXX)"
cleanup() {
    rm -f "$META_FILE"
}
trap cleanup EXIT INT TERM

python3 - "$PACKAGE" "$MACHINE_CODE" "$META_FILE" "$PACKAGE_SHA256" <<'PY'
import hashlib
import json
import os
import pathlib
import stat
import sys
import zipfile

package, machine_code, meta_path, expected_package_sha256 = sys.argv[1:]
max_package = 512 * 1024 * 1024
required = {"manifest.json", "topology.json", "nodes.json", "tags.json", "runtime-map.json"}

if os.path.getsize(package) > max_package:
    raise SystemExit("SCADA package compressed size exceeds 512 MiB")

package_digest = hashlib.sha256()
with open(package, "rb") as stream:
    for chunk in iter(lambda: stream.read(1024 * 1024), b""):
        package_digest.update(chunk)
actual_package_sha256 = package_digest.hexdigest()
expected_package_sha256 = expected_package_sha256.strip().lower()
if expected_package_sha256:
    if len(expected_package_sha256) != 64 or any(ch not in "0123456789abcdef" for ch in expected_package_sha256):
        raise SystemExit("invalid SCADA package SHA256")
    if actual_package_sha256 != expected_package_sha256:
        raise SystemExit(
            f"SCADA package SHA256 mismatch: expected={expected_package_sha256} actual={actual_package_sha256}"
        )

with zipfile.ZipFile(package, "r") as archive:
    entries = {}
    total = 0
    for info in archive.infolist():
        raw = info.filename
        if "\\" in raw:
            raise SystemExit(f"unsafe SCADA path: {raw}")
        raw_parts = raw.split("/")
        path = pathlib.PurePosixPath(raw)
        if raw.startswith("/") or any(part in ("", ".", "..") for part in raw_parts):
            raise SystemExit(f"unsafe SCADA path: {raw}")
        mode = info.external_attr >> 16
        if stat.S_ISLNK(mode):
            raise SystemExit(f"SCADA package may not contain symlinks: {raw}")
        if info.is_dir():
            continue
        normalized = str(path)
        if normalized in entries:
            raise SystemExit(f"duplicate SCADA file: {normalized}")
        total += info.file_size
        if total > max_package:
            raise SystemExit("SCADA package expanded size exceeds 512 MiB")
        entries[normalized] = info

    missing = required - entries.keys()
    if missing:
        raise SystemExit("SCADA package missing: " + ", ".join(sorted(missing)))
    if "checksums.json" not in entries:
        raise SystemExit("SCADA package missing checksums.json")

    checksums = json.loads(archive.read(entries["checksums.json"]))
    actual_files = set(entries) - {"checksums.json"}
    if set(checksums) != actual_files:
        unlisted = sorted(actual_files - set(checksums))
        stale = sorted(set(checksums) - actual_files)
        raise SystemExit(f"SCADA checksum manifest mismatch; unlisted={unlisted}, stale={stale}")
    for path, expected in checksums.items():
        actual = hashlib.sha256(archive.read(entries[path])).hexdigest()
        if actual.lower() != str(expected).lower():
            raise SystemExit(f"SCADA checksum failed: {path}")

    manifest = json.loads(archive.read(entries["manifest.json"]))
    topology = json.loads(archive.read(entries["topology.json"]))
    nodes = json.loads(archive.read(entries["nodes.json"]))
    if manifest.get("schemaVersion") != "2.0":
        raise SystemExit("unsupported SCADA schemaVersion")
    project_id = str(manifest.get("projectId", ""))
    version = str(manifest.get("packageVersion", ""))
    safe = set("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-")
    if not project_id or not version or any(ch not in safe for ch in project_id + version):
        raise SystemExit("invalid SCADA projectId or packageVersion")
    matching = [node for node in nodes if node.get("machineCode") == machine_code]
    if len(matching) != 1:
        raise SystemExit(f"SCADA package must contain machineCode exactly once: {machine_code}")
    mode = topology.get("mode", "integrated")
    if mode == "upperComputer":
        policy = topology.get("upperComputerOfflinePolicy") or {}
        if not policy.get("retainLocalSafetyRules", False):
            raise SystemExit("upper-computer package must retain local safety rules")

    with open(meta_path, "w", encoding="utf-8") as output:
        json.dump({
            "projectId": project_id,
            "version": version,
            "nodeId": matching[0].get("nodeId", ""),
            "mode": mode,
            "localScada": mode == "integrated" and manifest.get("packageRole", "project") != "edgeNode",
            "packageSha256": actual_package_sha256,
        }, output, ensure_ascii=True)
PY

PROJECT_ID="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["projectId"])' "$META_FILE")"
VERSION="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["version"])' "$META_FILE")"
NODE_ID="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["nodeId"])' "$META_FILE")"
LOCAL_SCADA="$(python3 -c 'import json,sys; print("true" if json.load(open(sys.argv[1]))["localScada"] else "false")' "$META_FILE")"
PACKAGE_SHA256_ACTUAL="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["packageSha256"])' "$META_FILE")"
if [ "$REQUIRE_LOCAL_SCADA" -eq 1 ] && [ "$LOCAL_SCADA" != "true" ]; then
    echo "SCADA package does not provide an integrated local project" >&2
    exit 1
fi
SCADA_SERVICE=""
if [ "$LOCAL_SCADA" = "true" ] && command -v systemctl >/dev/null 2>&1; then
    if systemctl list-unit-files ky-ems.service --no-legend 2>/dev/null | grep -q '^ky-ems.service'; then
        SCADA_SERVICE="ky-ems.service"
    elif systemctl list-unit-files local-display@.service --no-legend 2>/dev/null | grep -q '^local-display@.service'; then
        SCADA_SERVICE="local-display@monitor-service.service"
    fi
fi

echo "SCADA validation passed: project=$PROJECT_ID version=$VERSION node=$NODE_ID localScada=$LOCAL_SCADA sha256=$PACKAGE_SHA256_ACTUAL"
if [ "$DRY_RUN" -eq 1 ]; then
    exit 0
fi

mkdir -p "$SCADA_ROOT/releases" "$SCADA_ROOT/backup"
STAMP="$(date +%Y%m%d%H%M%S)"
RELEASE_NAME="${PROJECT_ID}-${VERSION}-${STAMP}"
STAGING="$SCADA_ROOT/releases/.${RELEASE_NAME}.staging"
RELEASE="$SCADA_ROOT/releases/$RELEASE_NAME"
CONFIG_BACKUP="$SCADA_ROOT/backup/monitor-service-${STAMP}.json"
OLD_TARGET=""
OLD_TARGET_RESOLVED=""
[ ! -e "$STAGING" ] || { echo "staging directory already exists: $STAGING" >&2; exit 1; }
[ ! -e "$RELEASE" ] || { echo "release directory already exists: $RELEASE" >&2; exit 1; }
if [ -L "$SCADA_ROOT/current" ]; then
    OLD_TARGET="$(readlink "$SCADA_ROOT/current")"
    OLD_TARGET_RESOLVED="$(readlink -f "$SCADA_ROOT/current")"
fi

rollback() {
    status=$?
    if [ "$status" -eq 0 ]; then
        return
    fi
    echo "SCADA install failed; rolling back SCADA release" >&2
    if [ -n "$OLD_TARGET" ]; then
        ln -sfn "$OLD_TARGET" "$SCADA_ROOT/current.rollback"
        mv -Tf "$SCADA_ROOT/current.rollback" "$SCADA_ROOT/current"
    else
        rm -f "$SCADA_ROOT/current"
    fi
    [ ! -f "$CONFIG_BACKUP" ] || cp -p "$CONFIG_BACKUP" "$APP_CONFIG"
    rm -rf "$STAGING" "$RELEASE"
    if [ "$RESTART" -eq 1 ] && [ -n "$SCADA_SERVICE" ]; then
        systemctl restart "$SCADA_SERVICE" >/dev/null 2>&1 || true
    fi
    exit "$status"
}
trap rollback EXIT INT TERM

mkdir -p "$STAGING"
python3 - "$PACKAGE" "$STAGING" <<'PY'
import pathlib
import sys
import zipfile

package, destination = sys.argv[1:]
root = pathlib.Path(destination)
with zipfile.ZipFile(package, "r") as archive:
    for info in archive.infolist():
        if info.is_dir():
            continue
        target = root.joinpath(*pathlib.PurePosixPath(info.filename).parts)
        target.parent.mkdir(parents=True, exist_ok=True)
        with archive.open(info, "r") as source, target.open("wb") as output:
            while True:
                chunk = source.read(65536)
                if not chunk:
                    break
                output.write(chunk)
PY
mv "$STAGING" "$RELEASE"
cp -p "$APP_CONFIG" "$CONFIG_BACKUP"

python3 - "$APP_CONFIG" "$LOCAL_SCADA" "$NODE_ID" "$SCADA_ROOT/current" <<'PY'
import json
import os
import sys
import tempfile

path, enabled, node_id, project_directory = sys.argv[1:]
with open(path, "r", encoding="utf-8") as source:
    root = json.load(source)
local_display = root.setdefault("localDisplay", {})
scada = local_display.setdefault("scada", {})
if enabled == "true":
    local_display["enabled"] = True
    local_display["renderer"] = "nativeQt"
scada.update({
    "enabled": enabled == "true",
    "projectDirectory": project_directory,
    "packageFile": "",
    "nodeId": node_id,
    "autoReload": True,
})
directory = os.path.dirname(path) or "."
fd, temporary = tempfile.mkstemp(prefix=".monitor-service.", suffix=".json", dir=directory)
try:
    with os.fdopen(fd, "w", encoding="utf-8") as output:
        json.dump(root, output, ensure_ascii=False, indent=2)
        output.write("\n")
        output.flush()
        os.fsync(output.fileno())
    os.replace(temporary, path)
finally:
    if os.path.exists(temporary):
        os.unlink(temporary)
PY

ln -sfn "releases/$RELEASE_NAME" "$SCADA_ROOT/current.new"
mv -Tf "$SCADA_ROOT/current.new" "$SCADA_ROOT/current"

if [ "$RESTART" -eq 1 ] && [ "$LOCAL_SCADA" = "true" ]; then
    [ -n "$SCADA_SERVICE" ] || { echo "local SCADA service was not found" >&2; exit 1; }
    systemctl restart "$SCADA_SERVICE"
    sleep 2
    FIRST_PID="$(systemctl show "$SCADA_SERVICE" -p MainPID --value)"
    systemctl is-active --quiet "$SCADA_SERVICE"
    [ -n "$FIRST_PID" ] && [ "$FIRST_PID" != "0" ] || { echo "local SCADA service has no main process" >&2; exit 1; }
    sleep 3
    SECOND_PID="$(systemctl show "$SCADA_SERVICE" -p MainPID --value)"
    systemctl is-active --quiet "$SCADA_SERVICE"
    [ "$SECOND_PID" = "$FIRST_PID" ] || { echo "local SCADA service restarted during health check" >&2; exit 1; }
fi

if [ -n "$STATE_FILE" ]; then
    STATE_DIR="$(dirname "$STATE_FILE")"
    mkdir -p "$STATE_DIR"
    STATE_TMP="${STATE_FILE}.tmp.$$"
    umask 077
    {
        echo "scadaRoot=$SCADA_ROOT"
        echo "scadaPreviousTarget=$OLD_TARGET_RESOLVED"
        echo "scadaCurrentTarget=$RELEASE"
        echo "scadaConfigBackup=$CONFIG_BACKUP"
        echo "scadaAppConfig=$APP_CONFIG"
        echo "scadaService=$SCADA_SERVICE"
        echo "scadaNodeId=$NODE_ID"
        echo "scadaProjectId=$PROJECT_ID"
        echo "scadaVersion=$VERSION"
        echo "scadaPackageSha256=$PACKAGE_SHA256_ACTUAL"
    } > "$STATE_TMP"
    mv "$STATE_TMP" "$STATE_FILE"
fi

trap - EXIT INT TERM
echo "SCADA release activated: $RELEASE"
