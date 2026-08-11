#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TEST_ROOT="${TMPDIR:-/tmp}/gateway-factory-scada-test-$$"
PAYLOAD_ROOT="$TEST_ROOT/payload"
MOCK_BIN="$TEST_ROOT/mock-bin"
SCADA_PACKAGE="$TEST_ROOT/site-a.kyscada"
FACTORY_PACKAGE="$TEST_ROOT/gateway-factory-full.tar.gz"
FACTORY_PACKAGE_SECOND="$TEST_ROOT/gateway-factory-full-second.tar.gz"
PACKAGE_EXTRACT_ROOT="$TEST_ROOT/package"
REFERENCE_ROOT="$TEST_ROOT/reference"
REFERENCE_SUMMARY="$TEST_ROOT/reference-summary.json"
FIXED_SCADA_PACKAGE_SHA256="9205c94b95fc6da153f53851967403d6d1772a44f6c60301cf401a1b97da2297"
FIXED_SCADA_CANONICAL_SHA256="0c56cf8fa47a646fcf98f7e073719f450dc47450a1d5d93dc527228c152dc263"
FIXED_SCADA_CONTENT_SHA256="d4ffa92b2ed6ed6355f83c55345a0ac3d9266f4b28450172cac981c2afba060c"
FIXED_SCADA_INPUT="${FACTORY_SCADA_TEST_PACKAGE:-}"

cleanup() {
  rm -rf "$TEST_ROOT"
}
trap cleanup EXIT INT TERM

[ "$(id -u)" = "0" ] || {
  echo "factory_scada_runtime_test must run as root to verify root:root canonical metadata" >&2
  exit 1
}

mkdir -p "$PAYLOAD_ROOT/build-aarch64" "$PAYLOAD_ROOT/ky-ems" "$MOCK_BIN"
for bin in ModbusRtu Dlt645Driver DioDriver CanDriver IecDriver MqttDriver EventEngine ComputeEngine \
  EmsParityCheck EmsClusterCoordinator SystemMonitor pointctl LocalDisplay QtDisplayBridge CameraService stress_runner; do
  printf '#!/bin/sh\nexit 0\n' >"$PAYLOAD_ROOT/build-aarch64/$bin"
  chmod +x "$PAYLOAD_ROOT/build-aarch64/$bin"
done
printf '#!/bin/sh\nexit 0\n' >"$PAYLOAD_ROOT/ky-ems/KY-EMS"
chmod +x "$PAYLOAD_ROOT/ky-ems/KY-EMS"

if [ -n "$FIXED_SCADA_INPUT" ]; then
  [ -f "$FIXED_SCADA_INPUT" ] || {
    echo "fixed SCADA test input does not exist: $FIXED_SCADA_INPUT" >&2
    exit 1
  }
  cp "$FIXED_SCADA_INPUT" "$SCADA_PACKAGE"
else
  python3 - "$SCADA_PACKAGE" <<'PY'
import hashlib
import json
import sys
import zipfile

documents = {
    "manifest.json": {
        "schemaVersion": "2.0",
        "projectId": "factory-site-a",
        "projectName": "factory test",
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
    "nodes.json": [{"nodeId": "edge-a", "machineCode": "COMM202600999"}],
    "tags.json": [],
    "runtime-map.json": [],
    "screens/overview.json": {"screenId": "overview", "widgets": []},
}
contents = {
    path: (json.dumps(value, ensure_ascii=True, indent=2) + "\n").encode("utf-8")
    for path, value in documents.items()
}
checksums = {path: hashlib.sha256(content).hexdigest() for path, content in contents.items()}
contents["checksums.json"] = (json.dumps(checksums, indent=2) + "\n").encode("utf-8")
with zipfile.ZipFile(sys.argv[1], "w", zipfile.ZIP_DEFLATED) as archive:
    for path, content in contents.items():
        archive.writestr(path, content)
PY
fi

SCADA_SHA256="$(sha256sum "$SCADA_PACKAGE" | awk '{print $1}')"
if [ -n "$FIXED_SCADA_INPUT" ] && [ "$SCADA_SHA256" != "$FIXED_SCADA_PACKAGE_SHA256" ]; then
  echo "fixed SCADA test input SHA256 mismatch: $SCADA_SHA256" >&2
  exit 1
fi

python3 - "$SCADA_PACKAGE" "$REFERENCE_ROOT" "$REFERENCE_SUMMARY" <<'PY'
import hashlib
import json
import os
import pathlib
import stat
import sys
import zipfile

package, destination, output = sys.argv[1:]
root = pathlib.Path(destination)
root.mkdir(parents=True)

with zipfile.ZipFile(package, "r") as archive:
    for info in archive.infolist():
        if info.is_dir():
            continue
        target = root.joinpath(*pathlib.PurePosixPath(info.filename).parts)
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(archive.read(info))

for directory, dirs, files in os.walk(root, topdown=False, followlinks=False):
    for name in files:
        path = pathlib.Path(directory, name)
        os.chown(path, 0, 0, follow_symlinks=False)
        os.chmod(path, 0o644, follow_symlinks=False)
    for name in dirs:
        path = pathlib.Path(directory, name)
        os.chown(path, 0, 0, follow_symlinks=False)
        os.chmod(path, 0o755, follow_symlinks=False)
os.chown(root, 0, 0, follow_symlinks=False)
os.chmod(root, 0o755, follow_symlinks=False)

def digest_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()

canonical_lines = []
content_lines = []
counts = {"files": 0, "directories": 0, "bytes": 0, "symlinks": 0, "other": 0}

def walk(path, relative):
    info = os.lstat(path)
    canonical = {
        "path": relative,
        "mode": format(stat.S_IMODE(info.st_mode), "04o"),
        "uid": info.st_uid,
        "gid": info.st_gid,
        "size": info.st_size,
    }
    content = {"path": relative, "size": info.st_size}
    if stat.S_ISREG(info.st_mode):
        kind = "file"
        counts["files"] += 1
        counts["bytes"] += info.st_size
        file_sha = digest_file(path)
        canonical["sha256"] = file_sha
        content["sha256"] = file_sha
    elif stat.S_ISDIR(info.st_mode):
        kind = "directory"
        counts["directories"] += 1
    elif stat.S_ISLNK(info.st_mode):
        kind = "symlink"
        counts["symlinks"] += 1
    else:
        kind = "other"
        counts["other"] += 1
    canonical["type"] = kind
    content["type"] = kind
    canonical_lines.append(json.dumps(canonical, ensure_ascii=True, sort_keys=True, separators=(",", ":")))
    content_lines.append(json.dumps(content, ensure_ascii=True, sort_keys=True, separators=(",", ":")))
    if kind == "directory":
        with os.scandir(path) as listing:
            children = sorted(listing, key=lambda item: item.name)
        for child in children:
            child_relative = child.name if relative == "." else relative + "/" + child.name
            walk(pathlib.Path(child.path), child_relative)

walk(root, ".")
documents = {
    name: json.loads((root / name).read_text(encoding="utf-8"))
    for name in ("manifest.json", "topology.json", "nodes.json", "checksums.json")
}
manifest = documents["manifest.json"]
topology = documents["topology.json"]
nodes = [node for node in documents["nodes.json"] if node.get("machineCode") == "COMM202600999"]
checksums = documents["checksums.json"]
if isinstance(checksums, dict):
    checksum_count = len(checksums)
else:
    raise SystemExit("test fixture checksums.json is not an object")
summary = {
    "canonical": hashlib.sha256(("\n".join(canonical_lines) + "\n").encode()).hexdigest(),
    "content": hashlib.sha256(("\n".join(content_lines) + "\n").encode()).hexdigest(),
    "fileCount": counts["files"],
    "directoryCount": counts["directories"],
    "totalBytes": counts["bytes"],
    "symlinkCount": counts["symlinks"],
    "otherCount": counts["other"],
    "checksumCount": checksum_count,
    "projectId": manifest.get("projectId"),
    "version": manifest.get("packageVersion"),
    "machineCode": nodes[0].get("machineCode") if len(nodes) == 1 else None,
    "nodeId": nodes[0].get("nodeId") if len(nodes) == 1 else None,
    "localScada": topology.get("mode", "integrated") == "integrated" and manifest.get("packageRole", "project") != "edgeNode",
}
pathlib.Path(output).write_text(json.dumps(summary, sort_keys=True) + "\n", encoding="utf-8")
PY

summary_value() {
  python3 - "$REFERENCE_SUMMARY" "$1" <<'PY'
import json
import sys
print(json.load(open(sys.argv[1], encoding="utf-8"))[sys.argv[2]])
PY
}

SCADA_CANONICAL_SHA256="$(summary_value canonical)"
SCADA_CONTENT_SHA256="$(summary_value content)"
if [ -n "$FIXED_SCADA_INPUT" ]; then
  [ "$SCADA_CANONICAL_SHA256" = "$FIXED_SCADA_CANONICAL_SHA256" ] || {
    echo "fixed SCADA canonical reference mismatch: $SCADA_CANONICAL_SHA256" >&2
    exit 1
  }
  [ "$SCADA_CONTENT_SHA256" = "$FIXED_SCADA_CONTENT_SHA256" ] || {
    echo "fixed SCADA content reference mismatch: $SCADA_CONTENT_SHA256" >&2
    exit 1
  }
  [ "$(summary_value fileCount)" = "27" ]
  [ "$(summary_value directoryCount)" = "2" ]
  [ "$(summary_value totalBytes)" = "1811862" ]
  [ "$(summary_value checksumCount)" = "26" ]
  [ "$(summary_value projectId)" = "ky-mobile-ems-COMM202600999" ]
  [ "$(summary_value version)" = "2.0.9-compact" ]
  [ "$(summary_value machineCode)" = "COMM202600999" ]
  [ "$(summary_value localScada)" = "True" ]
fi

if ALLOW_DIRTY_SOURCE=1 \
  FACTORY_BINARY_SOURCE_DIR="$PAYLOAD_ROOT/build-aarch64" \
  FACTORY_KY_EMS_BINARY="$PAYLOAD_ROOT/ky-ems/KY-EMS" \
  sh "$ROOT_DIR/deploy/build-factory-package.sh" --profile full --out "$TEST_ROOT/incomplete.tar.gz" \
    >/dev/null 2>&1; then
  echo "factory package accepted KY-EMS without a fixed SCADA input" >&2
  exit 1
fi
if ALLOW_DIRTY_SOURCE=1 \
  FACTORY_BINARY_SOURCE_DIR="$PAYLOAD_ROOT/build-aarch64" \
  FACTORY_KY_EMS_BINARY="$PAYLOAD_ROOT/ky-ems/KY-EMS" \
  SCADA_PROJECT_PACKAGE="$SCADA_PACKAGE" \
  SCADA_PROJECT_SHA256="$SCADA_SHA256" \
  SCADA_PROJECT_MACHINE_CODE=COMM202600999 \
  sh "$ROOT_DIR/deploy/build-factory-package.sh" --profile full --out "$TEST_ROOT/incomplete-scada-metadata.tar.gz" \
    >/dev/null 2>&1; then
  echo "factory package accepted embedded SCADA without canonical/content hashes" >&2
  exit 1
fi

build_factory_package() {
  output="$1"
  ALLOW_DIRTY_SOURCE=1 \
  FACTORY_BINARY_SOURCE_DIR="$PAYLOAD_ROOT/build-aarch64" \
  FACTORY_KY_EMS_BINARY="$PAYLOAD_ROOT/ky-ems/KY-EMS" \
  COMPONENT_VERSION=1.0.0-test \
  EDGE_TOOLCHAIN_ID=test-fixture \
  SCADA_PROJECT_PACKAGE="$SCADA_PACKAGE" \
  SCADA_PROJECT_SHA256="$SCADA_SHA256" \
  SCADA_PROJECT_MACHINE_CODE=COMM202600999 \
  SCADA_PROJECT_CANONICAL_SHA256="$SCADA_CANONICAL_SHA256" \
  SCADA_PROJECT_CONTENT_SHA256="$SCADA_CONTENT_SHA256" \
  sh "$ROOT_DIR/deploy/build-factory-package.sh" --profile full --out "$output"
}

build_factory_package "$FACTORY_PACKAGE"
sleep 1
build_factory_package "$FACTORY_PACKAGE_SECOND"
cmp "$FACTORY_PACKAGE" "$FACTORY_PACKAGE_SECOND" || {
  echo "factory package builds are not byte-identical" >&2
  exit 1
}

mkdir -p "$PACKAGE_EXTRACT_ROOT"
tar -xzf "$FACTORY_PACKAGE" -C "$PACKAGE_EXTRACT_ROOT"
PACKAGE_ROOT="$PACKAGE_EXTRACT_ROOT/gateway-factory-defaults"

cat >"$MOCK_BIN/systemctl" <<'SH'
#!/bin/sh
printf '%s\n' "$*" >>"$SYSTEMCTL_LOG"
exit 0
SH
chmod +x "$MOCK_BIN/systemctl"

MANIFEST_BACKUP="$TEST_ROOT/edge-package-manifest.json"
cp "$PACKAGE_ROOT/edge-package-manifest.json" "$MANIFEST_BACKUP"
python3 - "$PACKAGE_ROOT/edge-package-manifest.json" <<'PY'
import json
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
manifest = json.loads(path.read_text(encoding="utf-8"))
manifest["scadaProject"]["canonicalManifestSha256"] = "0" * 64
path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
PY
TAMPERED_ROOT="$TEST_ROOT/tampered"
mkdir -p "$TAMPERED_ROOT"
: >"$TAMPERED_ROOT/systemctl.log"
if PATH="$MOCK_BIN:$PATH" \
  SYSTEMCTL_LOG="$TAMPERED_ROOT/systemctl.log" \
  GATEWAY_HOME="$TAMPERED_ROOT/gateway" \
  BACKUP_DIR="$TAMPERED_ROOT/backup" \
  SOURCE_ROOT="$PACKAGE_ROOT" \
  FACTORY_DIR="$PACKAGE_ROOT/config/factory" \
  DEPLOY_DIR="$PACKAGE_ROOT/deploy" \
  FACTORY_PROMPT=0 \
  INIT_RUNTIME_MODE=ems \
  INIT_MACHINE_CODE=COMM202600999 \
  INIT_MQTT_BROKER=tcp://127.0.0.1:1883 \
  INIT_MQTT_TLS_ENABLED=false \
  START_SERVICES=0 \
  RESET_SHM=0 \
  INSTALL_SYSTEMD=0 \
  sh "$PACKAGE_ROOT/deploy/install-factory-config.sh" >/dev/null 2>&1; then
  cp "$MANIFEST_BACKUP" "$PACKAGE_ROOT/edge-package-manifest.json"
  echo "factory install accepted a mismatched embedded SCADA canonical hash" >&2
  exit 1
fi
cp "$MANIFEST_BACKUP" "$PACKAGE_ROOT/edge-package-manifest.json"
[ ! -e "$TAMPERED_ROOT/gateway/scada/current" ]

verify_install() {
  gateway_home="$1"
  manifest_path="$2"
  expected_summary="$3"
  python3 - "$gateway_home/scada/current" "$manifest_path" "$expected_summary" <<'PY'
import hashlib
import json
import os
import pathlib
import stat
import sys

current, manifest_path, expected_path = map(pathlib.Path, sys.argv[1:])
expected = json.loads(expected_path.read_text(encoding="utf-8"))
active = pathlib.Path(os.path.realpath(current))
if not current.is_symlink() or not active.is_dir():
    raise SystemExit("candidate SCADA current is not an active release symlink")

def digest_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()

canonical_lines = []
content_lines = []
counts = {"files": 0, "directories": 0, "bytes": 0, "symlinks": 0, "other": 0}

def walk(path, relative):
    info = os.lstat(path)
    canonical = {
        "path": relative,
        "mode": format(stat.S_IMODE(info.st_mode), "04o"),
        "uid": info.st_uid,
        "gid": info.st_gid,
        "size": info.st_size,
    }
    content = {"path": relative, "size": info.st_size}
    if stat.S_ISREG(info.st_mode):
        kind = "file"
        counts["files"] += 1
        counts["bytes"] += info.st_size
        file_sha = digest_file(path)
        canonical["sha256"] = file_sha
        content["sha256"] = file_sha
    elif stat.S_ISDIR(info.st_mode):
        kind = "directory"
        counts["directories"] += 1
    elif stat.S_ISLNK(info.st_mode):
        kind = "symlink"
        counts["symlinks"] += 1
    else:
        kind = "other"
        counts["other"] += 1
    canonical["type"] = kind
    content["type"] = kind
    canonical_lines.append(json.dumps(canonical, ensure_ascii=True, sort_keys=True, separators=(",", ":")))
    content_lines.append(json.dumps(content, ensure_ascii=True, sort_keys=True, separators=(",", ":")))
    if kind == "directory":
        with os.scandir(path) as listing:
            children = sorted(listing, key=lambda item: item.name)
        for child in children:
            child_relative = child.name if relative == "." else relative + "/" + child.name
            walk(pathlib.Path(child.path), child_relative)

walk(active, ".")
actual = {
    "canonical": hashlib.sha256(("\n".join(canonical_lines) + "\n").encode()).hexdigest(),
    "content": hashlib.sha256(("\n".join(content_lines) + "\n").encode()).hexdigest(),
    "fileCount": counts["files"],
    "directoryCount": counts["directories"],
    "totalBytes": counts["bytes"],
    "symlinkCount": counts["symlinks"],
    "otherCount": counts["other"],
}
for key, value in actual.items():
    if value != expected[key]:
        raise SystemExit(f"installed SCADA {key} mismatch: expected={expected[key]} actual={value}")
manifest = json.loads(manifest_path.read_text(encoding="utf-8"))["scadaProject"]
if manifest.get("canonicalManifestSha256") != expected["canonical"]:
    raise SystemExit("factory manifest canonical SHA256 mismatch")
if manifest.get("contentManifestSha256") != expected["content"]:
    raise SystemExit("factory manifest content SHA256 mismatch")
documents = {
    name: json.loads((active / name).read_text(encoding="utf-8"))
    for name in ("manifest.json", "topology.json", "nodes.json", "checksums.json")
}
checksums = documents["checksums.json"]
if not isinstance(checksums, dict) or len(checksums) != expected["checksumCount"]:
    raise SystemExit("installed SCADA checksum record count mismatch")
verified = 0
for relative, expected_sha256 in sorted(checksums.items()):
    path = active.joinpath(*pathlib.PurePosixPath(relative).parts)
    try:
        path.resolve().relative_to(active)
    except ValueError:
        raise SystemExit(f"installed SCADA checksum path escapes active release: {relative}")
    if not path.is_file() or path.is_symlink() or digest_file(path) != str(expected_sha256).lower():
        raise SystemExit(f"installed SCADA checksum mismatch: {relative}")
    verified += 1
project_manifest = documents["manifest.json"]
matching_nodes = [node for node in documents["nodes.json"] if node.get("machineCode") == expected["machineCode"]]
local_scada = (
    documents["topology.json"].get("mode", "integrated") == "integrated"
    and project_manifest.get("packageRole", "project") != "edgeNode"
)
if project_manifest.get("projectId") != expected["projectId"]:
    raise SystemExit("installed SCADA projectId mismatch")
if project_manifest.get("packageVersion") != expected["version"]:
    raise SystemExit("installed SCADA packageVersion mismatch")
if len(matching_nodes) != 1 or matching_nodes[0].get("nodeId") != expected["nodeId"]:
    raise SystemExit("installed SCADA machineCode/nodeId mismatch")
if local_scada is not expected["localScada"]:
    raise SystemExit("installed SCADA localScada mismatch")
print(
    "SCADA runtime canonical={canonical} content={content} files={fileCount} dirs={directoryCount} "
    "bytes={totalBytes} checksums={verified}/{expected}".format(
        **actual, verified=verified, expected=expected["checksumCount"]
    )
)
PY
}

run_install() {
  install_umask="$1"
  run_root="$TEST_ROOT/run-$install_umask"
  gateway_home="$run_root/gateway"
  systemd_root="$run_root/systemd"
  systemctl_log="$run_root/systemctl.log"
  mkdir -p "$run_root"
  : >"$systemctl_log"

  (
    umask "$install_umask"
    PATH="$MOCK_BIN:$PATH" \
    SYSTEMCTL_LOG="$systemctl_log" \
    GATEWAY_HOME="$gateway_home" \
    BACKUP_DIR="$run_root/backup" \
    SOURCE_ROOT="$PACKAGE_ROOT" \
    FACTORY_DIR="$PACKAGE_ROOT/config/factory" \
    DEPLOY_DIR="$PACKAGE_ROOT/deploy" \
    FACTORY_PROMPT=0 \
    INIT_RUNTIME_MODE=ems \
    INIT_MACHINE_CODE=COMM202600999 \
    INIT_MQTT_BROKER=tcp://127.0.0.1:1883 \
    INIT_MQTT_TLS_ENABLED=false \
    START_SERVICES=0 \
    RESET_SHM=0 \
    INSTALL_SYSTEMD=1 \
    SYSTEMD_UNIT_DIR="$systemd_root/system" \
    SYSTEM_DEFAULT_DIR="$systemd_root/default" \
    SYSTEMD_SYSTEM_CONF_DIR="$systemd_root/system.conf.d" \
    sh "$PACKAGE_ROOT/deploy/install-factory-config.sh"
  )

  [ -x "$gateway_home/bin/gateway-ky-ems-readiness.sh" ]
  for unit in "$systemd_root/system"/*.service; do
    [ "$(stat -c '%a' "$unit")" = "644" ] || {
      echo "systemd unit mode is not 0644: $unit" >&2
      exit 1
    }
  done
  start_count="$(grep -Ec '^(start|restart) ' "$systemctl_log" || true)"
  [ "$start_count" = "0" ] || {
    echo "START_SERVICES=0 unexpectedly started or restarted $start_count service(s)" >&2
    exit 1
  }

  [ -L "$gateway_home/scada/current" ]
  [ -f "$gateway_home/scada/current/manifest.json" ]
  [ -f "$gateway_home/scada/factory-install-state.txt" ]
  verify_install "$gateway_home" "$gateway_home/config/runtime/edge-package-manifest.json" "$REFERENCE_SUMMARY"
  python3 - "$gateway_home/config/runtime/apps/monitor-service.json" "$gateway_home/scada/current" \
    "$REFERENCE_SUMMARY" <<'PY'
import json
import sys

config_path, expected_project, summary_path = sys.argv[1:]
config = json.load(open(config_path, encoding="utf-8"))
expected = json.load(open(summary_path, encoding="utf-8"))
local_display = config["localDisplay"]
scada = local_display["scada"]
assert local_display["enabled"] is True
assert local_display["renderer"] == "nativeQt"
assert scada["enabled"] is True
assert scada["projectDirectory"] == expected_project
assert scada["packageFile"] == ""
assert scada["nodeId"] == expected["nodeId"]
assert expected["projectId"]
assert expected["version"]
assert expected["machineCode"] == "COMM202600999"
assert expected["localScada"] is True
assert expected["checksumCount"] > 0
PY
}

run_install 022
run_install 077

echo "factory_scada_runtime_test passed"
