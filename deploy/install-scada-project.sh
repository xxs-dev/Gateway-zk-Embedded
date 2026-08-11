#!/bin/sh
set -eu

PACKAGE=""
PACKAGE_SHA256=""
EXPECTED_CANONICAL_SHA256=""
EXPECTED_CONTENT_SHA256=""
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
  --canonical-manifest-sha256 SHA256
                      require the normalized active release canonical manifest hash
  --content-manifest-sha256 SHA256
                      require the normalized active release content manifest hash
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
        --canonical-manifest-sha256) EXPECTED_CANONICAL_SHA256="${2:-}"; shift 2 ;;
        --content-manifest-sha256) EXPECTED_CONTENT_SHA256="${2:-}"; shift 2 ;;
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
TREE_META_FILE="$(mktemp /tmp/gateway-scada-tree.XXXXXX)"
cleanup() {
    rm -f "$META_FILE" "$TREE_META_FILE"
}
trap cleanup EXIT INT TERM

python3 - "$PACKAGE" "$MACHINE_CODE" "$META_FILE" "$TREE_META_FILE" "$PACKAGE_SHA256" \
    "$EXPECTED_CANONICAL_SHA256" "$EXPECTED_CONTENT_SHA256" <<'PY'
import hashlib
import json
import os
import pathlib
import stat
import sys
import zipfile

(
    package,
    machine_code,
    meta_path,
    tree_meta_path,
    expected_package_sha256,
    expected_canonical_sha256,
    expected_content_sha256,
) = sys.argv[1:]
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

expected_canonical_sha256 = expected_canonical_sha256.strip().lower()
expected_content_sha256 = expected_content_sha256.strip().lower()
if bool(expected_canonical_sha256) != bool(expected_content_sha256):
    raise SystemExit("canonical/content manifest SHA256 values must be provided together")
for label, value in (
    ("canonical manifest", expected_canonical_sha256),
    ("content manifest", expected_content_sha256),
):
    if value and (len(value) != 64 or any(ch not in "0123456789abcdef" for ch in value)):
        raise SystemExit(f"invalid SCADA {label} SHA256")

with zipfile.ZipFile(package, "r") as archive:
    entries = {}
    seen_paths = set()
    total = 0
    for info in archive.infolist():
        raw = info.filename
        if "\\" in raw:
            raise SystemExit(f"unsafe SCADA path: {raw}")
        normalized_raw = raw[:-1] if raw.endswith("/") else raw
        raw_parts = normalized_raw.split("/")
        path = pathlib.PurePosixPath(normalized_raw)
        if raw.startswith("/") or not normalized_raw or any(part in ("", ".", "..") for part in raw_parts):
            raise SystemExit(f"unsafe SCADA path: {raw}")
        mode = info.external_attr >> 16
        file_type = stat.S_IFMT(mode)
        if file_type == stat.S_IFLNK:
            raise SystemExit(f"SCADA package may not contain symlinks: {raw}")
        allowed_types = (0, stat.S_IFDIR) if info.is_dir() else (0, stat.S_IFREG)
        if file_type not in allowed_types:
            raise SystemExit(f"SCADA package may not contain special files: {raw}")
        normalized = str(path)
        if normalized in seen_paths:
            raise SystemExit(f"duplicate SCADA entry: {normalized}")
        seen_paths.add(normalized)
        if info.is_dir():
            continue
        total += info.file_size
        if total > max_package:
            raise SystemExit("SCADA package expanded size exceeds 512 MiB")
        entries[normalized] = info

    file_paths = set(entries)
    for relative in entries:
        parent = pathlib.PurePosixPath(relative).parent
        while str(parent) != ".":
            if str(parent) in file_paths:
                raise SystemExit(f"SCADA path has a regular-file parent: {relative}")
            parent = parent.parent

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

    directories = {"."}
    for relative in entries:
        parent = pathlib.PurePosixPath(relative).parent
        while str(parent) != ".":
            directories.add(str(parent))
            parent = parent.parent
    children = {directory: [] for directory in directories}
    for directory in directories - {"."}:
        parent = str(pathlib.PurePosixPath(directory).parent)
        children[parent].append(directory)
    for relative in entries:
        parent = str(pathlib.PurePosixPath(relative).parent)
        children[parent].append(relative)

    canonical_lines = []
    content_lines = []
    counts = {"files": 0, "directories": 0, "bytes": 0}

    def collect_virtual(relative):
        if relative in directories:
            kind = "directory"
            size = 4096
            mode_value = "0755"
            counts["directories"] += 1
        else:
            kind = "file"
            info = entries[relative]
            size = info.file_size
            mode_value = "0644"
            counts["files"] += 1
            counts["bytes"] += size
        canonical = {
            "path": relative,
            "mode": mode_value,
            "uid": 0,
            "gid": 0,
            "size": size,
            "type": kind,
        }
        content = {"path": relative, "size": size, "type": kind}
        if kind == "file":
            file_sha256 = hashlib.sha256(archive.read(entries[relative])).hexdigest()
            canonical["sha256"] = file_sha256
            content["sha256"] = file_sha256
        canonical_lines.append(json.dumps(canonical, ensure_ascii=True, sort_keys=True, separators=(",", ":")))
        content_lines.append(json.dumps(content, ensure_ascii=True, sort_keys=True, separators=(",", ":")))
        if kind == "directory":
            for child in sorted(children[relative], key=lambda value: pathlib.PurePosixPath(value).name):
                collect_virtual(child)

    collect_virtual(".")
    canonical_sha256 = hashlib.sha256(("\n".join(canonical_lines) + "\n").encode()).hexdigest()
    content_sha256 = hashlib.sha256(("\n".join(content_lines) + "\n").encode()).hexdigest()
    if expected_canonical_sha256 and canonical_sha256 != expected_canonical_sha256:
        raise SystemExit(
            f"SCADA canonical manifest SHA256 mismatch: expected={expected_canonical_sha256} actual={canonical_sha256}"
        )
    if expected_content_sha256 and content_sha256 != expected_content_sha256:
        raise SystemExit(
            f"SCADA content manifest SHA256 mismatch: expected={expected_content_sha256} actual={content_sha256}"
        )
    pathlib.Path(tree_meta_path).write_text(json.dumps({
        "canonicalManifestSha256": canonical_sha256,
        "contentManifestSha256": content_sha256,
        "fileCount": counts["files"],
        "directoryCount": counts["directories"],
        "totalRegularFileBytes": counts["bytes"],
    }, ensure_ascii=True, sort_keys=True) + "\n", encoding="utf-8")

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
    ACTUAL_CANONICAL_SHA256="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["canonicalManifestSha256"])' "$TREE_META_FILE")"
    ACTUAL_CONTENT_SHA256="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["contentManifestSha256"])' "$TREE_META_FILE")"
    echo "SCADA verify-only normalized runtime passed: canonical=$ACTUAL_CANONICAL_SHA256 content=$ACTUAL_CONTENT_SHA256"
    exit 0
fi

[ "$(id -u)" = "0" ] || {
    echo "SCADA project installation requires root to enforce deterministic ownership" >&2
    exit 1
}
[ ! -L "$SCADA_ROOT" ] || { echo "SCADA root may not be a symlink: $SCADA_ROOT" >&2; exit 1; }
mkdir -p "$SCADA_ROOT/releases" "$SCADA_ROOT/backup"
for directory in "$SCADA_ROOT" "$SCADA_ROOT/releases" "$SCADA_ROOT/backup"; do
    [ ! -L "$directory" ] || { echo "SCADA runtime directory may not be a symlink: $directory" >&2; exit 1; }
    chown root:root "$directory"
    chmod 0755 "$directory"
done
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
python3 - "$PACKAGE" "$STAGING" "$PACKAGE_SHA256_ACTUAL" \
    "$EXPECTED_CANONICAL_SHA256" "$EXPECTED_CONTENT_SHA256" "$TREE_META_FILE" <<'PY'
import hashlib
import json
import os
import pathlib
import stat
import sys
import zipfile

(
    package,
    destination,
    expected_package_sha256,
    expected_canonical_sha256,
    expected_content_sha256,
    tree_meta_path,
) = sys.argv[1:]
root = pathlib.Path(destination)

if os.geteuid() != 0:
    raise SystemExit("SCADA extraction requires root")

def digest_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()

if digest_file(pathlib.Path(package)) != expected_package_sha256:
    raise SystemExit("SCADA package changed after validation")

root = root.resolve(strict=True)
with zipfile.ZipFile(package, "r") as archive:
    entries = {}
    for info in archive.infolist():
        raw = info.filename
        if "\\" in raw:
            raise SystemExit(f"unsafe SCADA path during extraction: {raw}")
        normalized_raw = raw[:-1] if raw.endswith("/") else raw
        parts = normalized_raw.split("/")
        if raw.startswith("/") or not normalized_raw or any(part in ("", ".", "..") for part in parts):
            raise SystemExit(f"unsafe SCADA path during extraction: {raw}")
        relative = pathlib.PurePosixPath(normalized_raw)
        normalized = str(relative)
        if normalized in entries:
            raise SystemExit(f"duplicate SCADA entry during extraction: {normalized}")
        mode = info.external_attr >> 16
        file_type = stat.S_IFMT(mode)
        if file_type == stat.S_IFLNK:
            raise SystemExit(f"SCADA package may not contain symlinks: {raw}")
        allowed_types = (0, stat.S_IFDIR) if info.is_dir() else (0, stat.S_IFREG)
        if file_type not in allowed_types:
            raise SystemExit(f"SCADA package may not contain special files: {raw}")
        entries[normalized] = info

    file_paths = {path for path, info in entries.items() if not info.is_dir()}
    for path in entries:
        parent = pathlib.PurePosixPath(path).parent
        while str(parent) != ".":
            if str(parent) in file_paths:
                raise SystemExit(f"SCADA path has a regular-file parent: {path}")
            parent = parent.parent

    for normalized, info in entries.items():
        if info.is_dir():
            continue
        target = root.joinpath(*pathlib.PurePosixPath(normalized).parts)
        try:
            target.relative_to(root)
        except ValueError:
            raise SystemExit(f"SCADA extraction path escapes staging root: {normalized}")
        target.parent.mkdir(mode=0o755, parents=True, exist_ok=True)
        parent = target.parent
        while parent != root.parent:
            parent_info = os.lstat(parent)
            if not stat.S_ISDIR(parent_info.st_mode) or stat.S_ISLNK(parent_info.st_mode):
                raise SystemExit(f"unsafe SCADA extraction parent: {normalized}")
            if parent == root:
                break
            parent = parent.parent
        flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
        if hasattr(os, "O_NOFOLLOW"):
            flags |= os.O_NOFOLLOW
        descriptor = os.open(target, flags, 0o600)
        with archive.open(info, "r") as source, os.fdopen(descriptor, "wb") as output:
            while True:
                chunk = source.read(65536)
                if not chunk:
                    break
                output.write(chunk)

def normalize_tree(path):
    info = os.lstat(path)
    if stat.S_ISLNK(info.st_mode):
        raise SystemExit(f"SCADA staging tree contains a symlink: {path.relative_to(root)}")
    if stat.S_ISDIR(info.st_mode):
        with os.scandir(path) as listing:
            children = list(listing)
        for child in children:
            normalize_tree(pathlib.Path(child.path))
        normalized_mode = 0o755
    elif stat.S_ISREG(info.st_mode):
        normalized_mode = 0o644
    else:
        raise SystemExit(f"SCADA staging tree contains a special file: {path.relative_to(root)}")
    if not hasattr(os, "O_NOFOLLOW"):
        raise SystemExit("SCADA metadata normalization requires O_NOFOLLOW support")
    flags = os.O_RDONLY | os.O_NOFOLLOW
    if stat.S_ISDIR(info.st_mode):
        flags |= getattr(os, "O_DIRECTORY", 0)
    descriptor = os.open(str(path), flags)
    try:
        opened = os.fstat(descriptor)
        if (opened.st_dev, opened.st_ino) != (info.st_dev, info.st_ino):
            raise SystemExit(f"SCADA staging entry changed during normalization: {path.relative_to(root)}")
        os.fchown(descriptor, 0, 0)
        os.fchmod(descriptor, normalized_mode)
    finally:
        os.close(descriptor)

normalize_tree(root)

canonical_lines = []
content_lines = []
counts = {"files": 0, "directories": 0, "bytes": 0}

def collect(path, relative):
    info = os.lstat(path)
    entry_size = 4096 if stat.S_ISDIR(info.st_mode) else info.st_size
    canonical = {
        "path": relative,
        "mode": format(stat.S_IMODE(info.st_mode), "04o"),
        "uid": info.st_uid,
        "gid": info.st_gid,
        "size": entry_size,
    }
    content = {"path": relative, "size": entry_size}
    if stat.S_ISREG(info.st_mode):
        kind = "file"
        counts["files"] += 1
        counts["bytes"] += info.st_size
        file_sha256 = digest_file(path)
        canonical["sha256"] = file_sha256
        content["sha256"] = file_sha256
    elif stat.S_ISDIR(info.st_mode):
        kind = "directory"
        counts["directories"] += 1
    else:
        raise SystemExit(f"SCADA normalized tree contains an unsafe entry: {relative}")
    canonical["type"] = kind
    content["type"] = kind
    canonical_lines.append(json.dumps(canonical, ensure_ascii=True, sort_keys=True, separators=(",", ":")))
    content_lines.append(json.dumps(content, ensure_ascii=True, sort_keys=True, separators=(",", ":")))
    if kind == "directory":
        with os.scandir(path) as listing:
            children = sorted(listing, key=lambda item: item.name)
        for child in children:
            child_relative = child.name if relative == "." else relative + "/" + child.name
            collect(pathlib.Path(child.path), child_relative)

collect(root, ".")
canonical_sha256 = hashlib.sha256(("\n".join(canonical_lines) + "\n").encode()).hexdigest()
content_sha256 = hashlib.sha256(("\n".join(content_lines) + "\n").encode()).hexdigest()
if expected_canonical_sha256 and canonical_sha256 != expected_canonical_sha256:
    raise SystemExit(
        f"SCADA canonical manifest SHA256 mismatch: expected={expected_canonical_sha256} actual={canonical_sha256}"
    )
if expected_content_sha256 and content_sha256 != expected_content_sha256:
    raise SystemExit(
        f"SCADA content manifest SHA256 mismatch: expected={expected_content_sha256} actual={content_sha256}"
    )
pathlib.Path(tree_meta_path).write_text(json.dumps({
    "canonicalManifestSha256": canonical_sha256,
    "contentManifestSha256": content_sha256,
    "fileCount": counts["files"],
    "directoryCount": counts["directories"],
    "totalRegularFileBytes": counts["bytes"],
}, ensure_ascii=True, sort_keys=True) + "\n", encoding="utf-8")
PY
ACTUAL_CANONICAL_SHA256="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["canonicalManifestSha256"])' "$TREE_META_FILE")"
ACTUAL_CONTENT_SHA256="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["contentManifestSha256"])' "$TREE_META_FILE")"
SCADA_FILE_COUNT="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["fileCount"])' "$TREE_META_FILE")"
SCADA_DIRECTORY_COUNT="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["directoryCount"])' "$TREE_META_FILE")"
SCADA_TOTAL_FILE_BYTES="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["totalRegularFileBytes"])' "$TREE_META_FILE")"
echo "SCADA normalized runtime verified: canonical=$ACTUAL_CANONICAL_SHA256 content=$ACTUAL_CONTENT_SHA256"
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
        echo "scadaStatus=ready"
        echo "scadaRoot=$SCADA_ROOT"
        echo "scadaPreviousTarget=$OLD_TARGET_RESOLVED"
        echo "scadaCurrentTarget=$RELEASE"
        echo "scadaConfigBackup=$CONFIG_BACKUP"
        echo "scadaAppConfig=$APP_CONFIG"
        echo "scadaService=$SCADA_SERVICE"
        echo "scadaMachineCode=$MACHINE_CODE"
        echo "scadaNodeId=$NODE_ID"
        echo "scadaProjectId=$PROJECT_ID"
        echo "scadaVersion=$VERSION"
        echo "scadaLocalScada=$LOCAL_SCADA"
        echo "scadaPackageSha256=$PACKAGE_SHA256_ACTUAL"
        echo "scadaCanonicalManifestSha256=$ACTUAL_CANONICAL_SHA256"
        echo "scadaContentManifestSha256=$ACTUAL_CONTENT_SHA256"
        echo "scadaAlgorithm=sorted-jsonl-tree-v1"
        echo "scadaDirectorySizePolicy=fixed-4096"
        echo "scadaFileCount=$SCADA_FILE_COUNT"
        echo "scadaDirectoryCount=$SCADA_DIRECTORY_COUNT"
        echo "scadaTotalRegularFileBytes=$SCADA_TOTAL_FILE_BYTES"
    } > "$STATE_TMP"
    mv "$STATE_TMP" "$STATE_FILE"
fi

trap - EXIT INT TERM
echo "SCADA release activated: $RELEASE"
