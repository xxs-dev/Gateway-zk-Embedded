#!/usr/bin/env python3
"""Verify a generic bundle and prepare a read-only, identity-bound local display."""
import argparse
import copy
import hashlib
import json
import math
import os
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import zipfile
import zlib
import stat
import uuid
from contextlib import contextmanager, ExitStack


# Original monitor app shares SystemMonitor's apply/pull single-file contract.
KIOSK_APP_MAX_BYTES = 5 * 1024 * 1024

KIOSK_DEFAULTS = {
    "width": 1280, "height": 720, "fullscreen": True, "setDisplayMode": True,
    "enabled": True, "requireDisplayConnected": True, "requireEdid": True,
    "displayOutput": "", "zoom": 1.0,
}


def kiosk_projection(root):
    """Explicit public fields only; never include a private-config fingerprint."""
    if not isinstance(root, dict) or not isinstance(root.get("localDisplay"), dict):
        raise ValueError("KIOSK_SETTINGS_INVALID")
    local = root["localDisplay"]
    port = local.get("port", 18080)
    if type(port) is not int or not 1 <= port <= 65535:
        raise ValueError("KIOSK_SETTINGS_INVALID")
    raw = local.get("kiosk", {})
    if not isinstance(raw, dict):
        raise ValueError("KIOSK_SETTINGS_INVALID")
    kiosk = {}
    for key, default in KIOSK_DEFAULTS.items():
        value = raw.get(key, default)
        if type(default) is bool:
            valid = type(value) is bool
        elif type(default) is int:
            valid = type(value) is int and 1 <= value <= 16384
        elif key == "zoom":
            valid = type(value) in (int, float) and 0.1 <= value <= 8 and math.isfinite(value)
        else:
            valid = isinstance(value, str) and re.fullmatch(r"[A-Za-z0-9_.:-]{0,64}", value) is not None
        if not valid:
            raise ValueError("KIOSK_SETTINGS_INVALID")
        kiosk[key] = value
    pages = local.get("pages", [])
    if not isinstance(pages, list):
        raise ValueError("KIOSK_SETTINGS_INVALID")
    page = "overview"
    if pages:
        if not isinstance(pages[0], dict):
            raise ValueError("KIOSK_SETTINGS_INVALID")
        page = pages[0].get("pageCode", "overview")
    if not isinstance(page, str) or re.fullmatch(r"[A-Za-z0-9_.-]{1,64}", page) is None:
        raise ValueError("KIOSK_SETTINGS_INVALID")
    return {"localDisplay": {"port": port, "kiosk": kiosk, "pages": [{"pageCode": page}]}}


def kiosk_selected(root):
    """Configuration selection only, matching gateway-services; no hardware probe."""
    def enabled(value, default):
        if isinstance(value, bool):
            return value
        if value is None:
            return default
        return str(value).strip().lower() in ("1", "true", "yes", "on")

    if not isinstance(root, dict):
        raise ValueError("KIOSK_SETTINGS_INVALID")
    local = root.get("localDisplay")
    if local is None:
        return False
    if not isinstance(local, dict):
        raise ValueError("KIOSK_SETTINGS_INVALID")
    if not enabled(local.get("enabled"), False):
        return False
    renderer = str(local.get("renderer") or local.get("mode") or "webkit").strip().lower()
    if renderer in ("nativeqt", "qt", "ky-ems", "kyems"):
        return False
    kiosk = local.get("kiosk") or {}
    if not isinstance(kiosk, dict):
        raise ValueError("KIOSK_SETTINGS_INVALID")
    return enabled(kiosk.get("enabled"), True)


def kiosk_json(raw):
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError("duplicate")
            result[key] = value
        return result

    def nonfinite(value):
        raise ValueError("nonfinite")

    def finite_number(value):
        number = float(value)
        if not math.isfinite(number):
            raise ValueError("nonfinite")
        return number

    try:
        return json.loads(raw.decode("utf-8"), object_pairs_hook=unique,
                          parse_constant=nonfinite, parse_float=finite_number)
    except (ValueError, UnicodeError, RecursionError):
        raise ValueError("KIOSK_JSON_INVALID") from None


def kiosk_directory_policy(fd, public=False, ancestor=False):
    info = os.fstat(fd)
    mode = stat.S_IMODE(info.st_mode)
    if (not stat.S_ISDIR(info.st_mode) or info.st_uid != 0 or mode & 0o022 or
            (public and mode != 0o755) or (ancestor and not mode & 0o001)):
        raise ValueError("KIOSK_PARENT_POLICY")


def kiosk_file_policy(fd):
    info = os.fstat(fd)
    if (not stat.S_ISREG(info.st_mode) or info.st_uid != 0 or
            info.st_mode & 0o022 or info.st_nlink != 1):
        raise ValueError("KIOSK_FILE_POLICY")
    return info


def kiosk_read(fd, limit):
    kiosk_file_policy(fd)
    chunks = []
    count = 0
    while True:
        chunk = os.read(fd, min(65536, limit + 1 - count))
        count += len(chunk)
        if count > limit:
            raise ValueError("KIOSK_INPUT_TOO_LARGE")
        if not chunk:
            return b"".join(chunks)
        chunks.append(chunk)


def kiosk_read_source(source, limit, allow_missing=False):
    if os.name != "posix" or os.geteuid() != 0:
        raise ValueError("KIOSK_ROOT_POSIX_REQUIRED")
    try:
        fd = os.open(str(source), kiosk_flags())
    except FileNotFoundError:
        if allow_missing:
            return None
        raise
    try:
        before = os.fstat(fd)
        raw = kiosk_read(fd, limit)
        after = os.fstat(fd)
        if (before.st_size, before.st_mtime_ns, before.st_ctime_ns) != (
                after.st_size, after.st_mtime_ns, after.st_ctime_ns):
            raise ValueError("KIOSK_INPUT_CHANGED")
        return raw
    finally:
        os.close(fd)


def check_kiosk_source(args):
    kiosk_read_source(args.source, 256 * 1024)


def check_kiosk_app(args):
    raw = kiosk_read_source(args.source, KIOSK_APP_MAX_BYTES, allow_missing=True)
    if raw is not None:
        root = kiosk_json(raw)
        if kiosk_selected(root):
            kiosk_projection(root)


def kiosk_instance(value):
    if re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_-]{0,63}", value) is None:
        raise ValueError("KIOSK_INSTANCE_INVALID")
    return value


def kiosk_flags(directory=False):
    return os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC | os.O_NONBLOCK | (os.O_DIRECTORY if directory else 0)


@contextmanager
def kiosk_home(home, allow_missing=False):
    # Walk from root using held no-follow directory handles; never resolve links.
    if os.name != "posix" or os.geteuid() != 0:
        raise ValueError("KIOSK_ROOT_POSIX_REQUIRED")
    path = Path(home)
    if not path.is_absolute() or ".." in path.parts or len(path.parts) < 3:
        raise ValueError("KIOSK_HOME_INVALID")
    fd = os.open("/", kiosk_flags(True))
    try:
        kiosk_directory_policy(fd, ancestor=True)
        parts = path.parts[1:]
        for position, part in enumerate(parts):
            try:
                next_fd = os.open(part, kiosk_flags(True), dir_fd=fd)
            except FileNotFoundError:
                if allow_missing and position == len(parts) - 1:
                    yield fd, part, None
                    return
                raise
            try:
                kiosk_directory_policy(next_fd, public=position == len(parts) - 1, ancestor=True)
            except Exception:
                os.close(next_fd)
                raise
            os.close(fd)
            fd = next_fd
        yield None, None, fd
    finally:
        os.close(fd)


def kiosk_check_slot(directory, name):
    try:
        fd = os.open(name, kiosk_flags(), dir_fd=directory)
    except FileNotFoundError:
        return
    try:
        kiosk_file_policy(fd)
    finally:
        os.close(fd)


def check_kiosk_parents(args):
    with kiosk_home(args.home, allow_missing=True) as (_, __, home_fd):
        if home_fd is None:
            return
        for name in ("bin", "kiosk"):
            try:
                fd = os.open(name, kiosk_flags(True), dir_fd=home_fd)
            except FileNotFoundError:
                continue
            try:
                kiosk_directory_policy(fd, public=True)
                if name == "bin":
                    kiosk_check_slot(fd, "local-kiosk.py")
            finally:
                os.close(fd)


def kiosk_create_directory(parent, name):
    try:
        os.mkdir(name, 0o700, dir_fd=parent)
    except FileExistsError:
        fd = os.open(name, kiosk_flags(True), dir_fd=parent)
        try:
            kiosk_directory_policy(fd, public=True)
        finally:
            os.close(fd)
        return
    fd = os.open(name, kiosk_flags(True), dir_fd=parent)
    try:
        kiosk_directory_policy(fd)
        os.fchmod(fd, 0o755)
        os.fsync(fd)
    finally:
        os.close(fd)
    os.fsync(parent)


def prepare_kiosk_parents(args):
    check_kiosk_parents(args)
    with kiosk_home(args.home, allow_missing=True) as (parent, name, home_fd):
        if home_fd is None:
            kiosk_create_directory(parent, name)
    with kiosk_home(args.home) as (_, __, home_fd):
        for name in ("bin", "kiosk"):
            kiosk_create_directory(home_fd, name)


@contextmanager
def kiosk_child_directory(parent, name):
    fd = os.open(name, kiosk_flags(True), dir_fd=parent)
    try:
        kiosk_directory_policy(fd)
        yield fd
    finally:
        os.close(fd)


def kiosk_publish(directory, name, raw, mode):
    kiosk_check_slot(directory, name)
    temporary = ".kiosk-" + uuid.uuid4().hex
    fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW | os.O_CLOEXEC,
                 0o600, dir_fd=directory)
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(raw)
            stream.flush()
            os.fchmod(stream.fileno(), mode)
            os.fsync(stream.fileno())
        os.replace(temporary, name, src_dir_fd=directory, dst_dir_fd=directory)
        os.fsync(directory)
    finally:
        try:
            os.unlink(temporary, dir_fd=directory)
        except FileNotFoundError:
            pass


def project_kiosk(args):
    instance = kiosk_instance(args.instance)
    with kiosk_home(args.home) as (_, __, home_fd):
        with kiosk_child_directory(home_fd, "config") as config:
            with kiosk_child_directory(config, "runtime") as runtime:
                with kiosk_child_directory(runtime, "apps") as apps:
                    try:
                        fd = os.open(instance + ".json", kiosk_flags(), dir_fd=apps)
                    except FileNotFoundError:
                        if getattr(args, "if_selected", False):
                            return
                        raise
                    try:
                        before = os.fstat(fd)
                        raw = kiosk_read(fd, KIOSK_APP_MAX_BYTES)
                        after = os.fstat(fd)
                        if (before.st_size, before.st_mtime_ns, before.st_ctime_ns) != (
                                after.st_size, after.st_mtime_ns, after.st_ctime_ns):
                            raise ValueError("KIOSK_INPUT_CHANGED")
                    finally:
                        os.close(fd)
        root = kiosk_json(raw)
        if getattr(args, "if_selected", False) and not kiosk_selected(root):
            return
        value = kiosk_projection(root)
        public = (json.dumps(value, ensure_ascii=True, sort_keys=True) + "\n").encode("utf-8")
        with kiosk_child_directory(home_fd, "kiosk") as output:
            kiosk_directory_policy(output, public=True)
            kiosk_publish(output, instance + ".json", public, 0o644)


def install_kiosk_script(args):
    # Source is the approved deploy member, not an arbitrary output path.
    check_kiosk_parents(args)
    raw = kiosk_read_source(args.source, 256 * 1024)
    with kiosk_home(args.home) as (_, __, home_fd):
        with kiosk_child_directory(home_fd, "bin") as output:
            kiosk_directory_policy(output, public=True)
            kiosk_publish(output, "local-kiosk.py", raw, 0o755)


def tls_app_bytes(raw, ca_file, cert_file, key_file, monitor=False):
    """The stage and enrollment provider must use exactly the same bytes."""
    if monitor and len(raw) > KIOSK_APP_MAX_BYTES:
        raise ValueError("KIOSK_INPUT_TOO_LARGE")
    app = kiosk_json(raw)
    if not isinstance(app, dict):
        raise ValueError("TLS_APP_INVALID")
    mqtt = app.get("mqtt")
    if not isinstance(mqtt, dict):
        return raw
    tls = mqtt.setdefault("tls", {})
    if not isinstance(tls, dict):
        raise ValueError("TLS_APP_INVALID")
    tls.update(enabled=True, caFile=ca_file, certFile=cert_file,
               keyFile=key_file, insecureSkipVerify=False)
    result = (json.dumps(app, ensure_ascii=False, indent=2) + "\n").encode("utf-8")
    if monitor and len(result) > KIOSK_APP_MAX_BYTES:
        raise ValueError("KIOSK_INPUT_TOO_LARGE")
    return result


def tls_snapshot(fd):
    info = kiosk_file_policy(fd)
    return (getattr(info, "st_dev", None), getattr(info, "st_ino", None),
            info.st_size, info.st_mtime_ns, info.st_ctime_ns, info.st_mode)


def prepare_tls_apps(args):
    if os.name != "posix" or os.geteuid() != 0:
        raise ValueError("KIOSK_ROOT_POSIX_REQUIRED")
    # Private stage directories may be 0700; unlike the public kiosk directory,
    # they do not need world traversal. All app operations use held handles.
    directory = os.open(str(args.app_config.parent), kiosk_flags(True))
    try:
        kiosk_directory_policy(directory)
        names = sorted(set([args.app_config.name] +
                           [name for name in os.listdir(directory) if name.endswith(".json")]))
        with ExitStack() as held:
            plans = []
            for name in names:
                try:
                    fd = os.open(name, kiosk_flags(), dir_fd=directory)
                except FileNotFoundError:
                    continue
                held.callback(os.close, fd)
                before = tls_snapshot(fd)
                raw = kiosk_read(fd, KIOSK_APP_MAX_BYTES if name == "monitor-service.json" else before[2])
                result = tls_app_bytes(raw, args.ca_file, args.cert_file, args.key_file,
                                       monitor=name == "monitor-service.json")
                if before != tls_snapshot(fd):
                    raise ValueError("KIOSK_INPUT_CHANGED")
                if args.require_prepared and raw != result:
                    raise ValueError("TLS_APP_NOT_PREPARED")
                plans.append((name, fd, before, raw, result))
            # Validate every candidate before publishing any file. A provider in
            # production-init never rewrites: any drift is a failure, not a fixup.
            for name, fd, before, raw, result in plans:
                current = os.open(name, kiosk_flags(), dir_fd=directory)
                try:
                    if before != tls_snapshot(fd) or before != tls_snapshot(current):
                        raise ValueError("KIOSK_INPUT_CHANGED")
                finally:
                    os.close(current)
            if not args.require_prepared:
                for name, fd, before, raw, result in plans:
                    if raw != result:
                        kiosk_publish(directory, name, result, stat.S_IMODE(before[5]))
    finally:
        os.close(directory)

QT_WIDGET_RESOURCE = "config/generic-scada-widgets.json"


def qt_widgets(root, manifest):
    path = root / QT_WIDGET_RESOURCE
    expected = manifest.get("files", {}).get(QT_WIDGET_RESOURCE)
    if not isinstance(expected, str) or not re.fullmatch("[0-9a-f]{64}", expected):
        raise ValueError("QT_WIDGET_RESOURCE_UNBOUND")
    if path.is_symlink() or not path.is_file():
        raise ValueError("QT_WIDGET_RESOURCE_MISSING")
    with path.open("rb") as stream:
        raw = stream.read(65537)
    if len(raw) > 65536 or hashlib.sha256(raw).hexdigest() != expected:
        raise ValueError("QT_WIDGET_RESOURCE_MISMATCH")
    resources = read(root / "edge-package-manifest.json").get("resources", {})
    if resources.get(QT_WIDGET_RESOURCE) != {"sha256": expected, "sizeBytes": len(raw)}:
        raise ValueError("QT_WIDGET_RESOURCE_MANIFEST_MISMATCH")
    try:
        def unique(pairs):
            value = {}
            for key, child in pairs:
                if key in value:
                    raise ValueError("duplicate widget field")
                value[key] = child
            return value
        widgets = json.loads(raw, object_pairs_hook=unique)
        for kind in ("qtLabel", "qtValue", "qtButton"):
            widget = widgets[kind]
            if widget["type"] != kind or widget["bindings"] != [] or widget["stateRules"] != [] or widget["action"] is not None:
                raise ValueError("bound or incorrect prototype")
            if not isinstance(widget["properties"], dict) or widget["visible"] is not True or type(widget["zIndex"]) is not int:
                raise ValueError("invalid prototype schema")
            geometry = widget["geometry"]
            if any(type(geometry[key]) not in (int, float) or not math.isfinite(geometry[key]) for key in ("x", "y", "width", "height")):
                raise ValueError("invalid prototype geometry")
            if geometry["width"] <= 0 or geometry["height"] <= 0:
                raise ValueError("invalid prototype dimensions")
    except (ValueError, KeyError, TypeError) as error:
        raise ValueError("QT_WIDGET_RESOURCE_INVALID") from error
    return widgets, raw


def install_widgets(args):
    _, raw = qt_widgets(args.package_root, read(args.package_root / "generic-package.json"))
    target = args.home / QT_WIDGET_RESOURCE
    target.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=".qt-widgets-", dir=str(target.parent))
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(raw)
            stream.flush()
            os.fsync(stream.fileno())
        os.chmod(temporary, 0o644)
        os.replace(temporary, str(target))
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def read(path):
    return json.loads(Path(path).read_text(encoding="utf-8"))


def write(path, value, ensure_ascii=True):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=".generic-", dir=str(path.parent))
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as output:
            json.dump(value, output, ensure_ascii=ensure_ascii, indent=2)
            output.write("\n")
        os.replace(temporary, str(path))
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def verify_files(root, files):
    for name, expected in files.items():
        path = root / name
        if path.is_symlink() or root.resolve() not in path.resolve().parents:
            raise ValueError("unsafe bundle path: " + name)
        if not path.is_file() or sha(path) != expected:
            raise ValueError("bundle hash mismatch: " + name)


QT_KEYS = ("DISPLAY", "XAUTHORITY", "XDG_RUNTIME_DIR", "QT_QPA_PLATFORM", "QT_QPA_PLATFORM_PLUGIN_PATH")


def qt_environment(home):
    result = {key: os.environ[key] for key in QT_KEYS if os.environ.get(key)}
    name = os.environ.get("INIT_QT_ENV_FILE", "")
    path = Path(name) if name else home / "config/runtime/qt-display.env"
    if name or path.exists():
        metadata = path.stat()
        if path.is_symlink() or metadata.st_uid != 0 or metadata.st_mode & 0o022:
            raise ValueError("Qt environment file must be root-owned, not symlinked or writable by group/others")
        for line in path.read_text().splitlines():
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            key, separator, value = line.partition("=")
            if not separator or key not in QT_KEYS or any(character in value for character in "\"'`$\\\n\r"):
                raise ValueError("Qt environment requires literal KEY=value from the documented allowlist")
            result[key] = value
    for value in result.values():
        if any(character in value for character in "\"'`$\\\n\r"):
            raise ValueError("Qt environment values must be literal single-line values")
    return result


def preflight(args):
    root = args.package_root
    generic = read(root / "generic-package.json")
    verify_files(root, generic["files"])
    # Windows uploads deploy files separately. They must match this exact bundle.
    for name, expected in generic["files"].items():
        if name.startswith("deploy/"):
            path = args.deploy_dir / Path(name).name
            if not path.is_file() or sha(path) != expected:
                raise ValueError("paired deploy script mismatch: " + Path(name).name)
    if args.mode == "agc_avc":
        overlay = read(root / "agc-avc-runtime-manifest.json")
        if overlay.get("sourceCommit") != generic["binarySourceCommit"]:
            raise ValueError("AGC overlay source does not match generic programs")
        verify_files(root, overlay["files"])
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,95}", args.machine) or args.machine == "GW_FACTORY_001":
        raise ValueError("generic initialization requires an explicit non-placeholder machine code")
    if args.profile == "base" and args.mode == "ems":
        raise ValueError("base profile cannot initialize EMS mode")
    selection = read(args.manifest) if args.manifest else {"requiredDrivers": []}
    if generic.get("initializationKind") == "generic-uncommissioned":
        selection["initializationKind"] = "generic-uncommissioned"
    if args.profile == "project" and not args.manifest:
        raise ValueError("project profile requires --manifest")
    names = set()
    for key in ("requiredDrivers", "components", "runtimeComponents"):
        for item in selection.get(key, []):
            name = item if isinstance(item, str) else item.get("binary", item.get("name", item.get("id", "")))
            names.add(name)
    available = {item["binary"] for item in read(root / "edge-package-manifest.json")["components"]}
    if args.mode == "agc_avc":
        available.add("AgcAvcController")
    if names - available:
        raise ValueError("unknown or unavailable selected drivers: " + repr(sorted(names - available)))
    qt = args.qt == "1" or (args.qt == "auto" and args.profile == "project" and "KY-EMS" in names)
    if qt and args.profile == "base":
        raise ValueError("base profile does not support --local-qt-display; use project or full")
    if qt:
        qt_widgets(root, generic)
        for binary in ("KY-EMS", "QtDisplayBridge"):
            if binary not in available:
                raise ValueError("local Qt dependency missing: " + binary)
            if binary not in names:
                selection.setdefault("requiredDrivers", []).append({"binary": binary})
        environment = qt_environment(args.home)
        if args.start.lower() in ("1", "true", "yes", "y", "on"):
            subprocess.run(["sh", str(args.deploy_dir / "gateway-qt-run.sh"), "--check"], check=True,
                           env=dict(os.environ, **environment,
                                    GATEWAY_QT_BINARY=str(root / "ky-ems/KY-EMS"),
                                    GATEWAY_QT_BRIDGE=str(root / "build-aarch64/QtDisplayBridge")))
        write(args.selection.parent / "generic-qt-environment.json", environment)
    write(args.selection, selection)
    print("1" if qt else "0")


def relocate(value, home, machine, source_home="/opt/modbus-gateway"):
    if isinstance(value, dict):
        return {key: (machine if key == "machineCode" else relocate(item, home, machine, source_home))
                for key, item in value.items()}
    if isinstance(value, list):
        return [relocate(item, home, machine, source_home) for item in value]
    source_prefixes = (str(source_home),)
    if isinstance(source_home, Path):
        source_prefixes += (source_home.as_posix(),)
    for prefix in source_prefixes:
        if isinstance(value, str) and value.startswith((prefix + "/", prefix + os.sep)):
            return str(home) + value[len(prefix):]
    return value


def inventory(home, machine):
    tags, routes, seen_indexes, seen_tags = [], [], set(), set()
    app = read(home / "config/runtime/apps/monitor-service.json")
    device_root = (home / "config/runtime/devices").resolve()
    for name in app.get("deviceConfigFiles", []):
        path = Path(name).resolve()
        if device_root not in path.parents:
            raise ValueError("device reference outside runtime devices")
        if not path.is_file():
            raise ValueError("missing device config: " + str(path))
        device = read(path)
        if not device.get("enabled", True):
            continue
        store = device.get("memoryStore", {}).get("sharedMemoryName", "")
        for meter in device.get("meters", []):
            if not meter.get("enabled", True):
                continue
            meter_code = str(meter.get("meterCode", ""))
            for point in meter.get("points", []):
                if not point.get("enabled", True):
                    continue
                index = point.get("index")
                code = str(point.get("pointCode", ""))
                tag = meter_code + "." + code
                if not meter_code or not code or not store or not isinstance(index, int) or isinstance(index, bool) or index <= 0:
                    raise ValueError("incomplete runtime point identity: " + tag)
                if index in seen_indexes or tag in seen_tags:
                    raise ValueError("ambiguous runtime point identity/index: " + tag)
                seen_indexes.add(index)
                seen_tags.add(tag)
                tags.append({"tagId": tag, "nodeId": "edge-1", "deviceId": meter_code,
                             "meterCode": meter_code, "pointCode": code, "access": "read",
                             "displayName": str(point.get("name", code)), "unit": str(point.get("unit", "")),
                             "dataType": "float64", "indexFallback": index})
                routes.append({"nodeId": "edge-1", "tagId": tag, "sharedMemoryName": store,
                               "index": index, "writable": False, "dataType": "float64"})
    return tags, routes


def background_png():
    # The Qt renderer treats screen.background as a QPixmap path, not a color.
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 1, 1, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(bytes((0, 244, 246, 248)))) + chunk(b"IEND", b""))


def make_project(home, machine, output, resources=None):
    tags, routes = inventory(home, machine)
    # Reuse the product Qt widget schema, with no inherited site tags or controls.
    prototypes = read((resources or home) / "config/generic-scada-widgets.json")
    pages = max(1, (len(tags) + 15) // 16)
    documents = {
        "manifest.json": {"schemaVersion": "2.0", "projectId": "gateway-" + machine,
                          "packageVersion": "1.0.0", "entryScreen": "Points1", "packageRole": "project"},
        "topology.json": {"mode": "integrated", "scadaHost": "edge", "emsHost": "edge",
                          "dataTransport": "sharedMemory", "offlinePolicy": "continueLocal"},
        "nodes.json": [{"nodeId": "edge-1", "machineCode": machine, "displayName": machine, "roles": ["acquisition"]}],
        "tags.json": tags, "runtime-map.json": routes,
    }
    for page in range(pages):
        widgets = []
        def widget(kind, identity, title, x, y, width, height):
            value = copy.deepcopy(prototypes[kind])
            value.update(widgetId=identity, title=title, geometry=dict(x=x, y=y, width=width, height=height))
            value["properties"]["qtText"] = title
            value["properties"]["qtFontSize"] = 18
            value["properties"]["qtTextColor"] = "#17212B"
            widgets.append(value)
            return value
        widget("qtLabel", "title", machine, 32, 24, 960, 40)
        widget("qtLabel", "count", str(len(tags)) + " points", 32, 76, 500, 28)
        for row, tag in enumerate(tags[page * 16:(page + 1) * 16]):
            y = 125 + row * 30
            widget("qtLabel", "name-" + str(row), tag["meterCode"] + " / " + tag["displayName"], 32, y, 840, 28)
            value = widget("qtValue", "value-" + str(row), "--", 900, y, 160, 28)
            value["bindings"] = [{"nodeId": "edge-1", "tagId": tag["tagId"], "slot": "value"}]
            widget("qtLabel", "unit-" + str(row), tag["unit"], 1080, y, 150, 28)
        for label, target, x in (("<", page - 1, 950), (">", page + 1, 1130)):
            if 0 <= target < pages:
                button = widget("qtButton", "nav-" + str(target), label, x, 655, 80, 40)
                button["action"] = {"type": "navigate", "targetScreen": "Points" + str(target + 1)}
        widget("qtLabel", "page", str(page + 1) + " / " + str(pages), 1040, 660, 85, 30)
        documents["screens/Points" + str(page + 1) + ".json"] = {
            "screenId": "Points" + str(page + 1), "title": machine, "width": 1280, "height": 720,
            "background": "assets/generic-background.png", "widgets": widgets}
    payloads = {name: (json.dumps(value, ensure_ascii=True, indent=2) + "\n").encode() for name, value in documents.items()}
    payloads["assets/generic-background.png"] = background_png()
    payloads["checksums.json"] = json.dumps({name: hashlib.sha256(data).hexdigest() for name, data in payloads.items()}).encode()
    with zipfile.ZipFile(str(output), "w", zipfile.ZIP_DEFLATED) as archive:
        for name, data in payloads.items():
            archive.writestr(name, data)


def relocate_runtime(home, target, machine, source_home="/opt/modbus-gateway", monitor_ascii=True):
    for path in (home / "config/runtime").rglob("*.json"):
        write(path, relocate(read(path), target, machine, source_home),
              ensure_ascii=monitor_ascii if path == home / "config/runtime/apps/monitor-service.json" else True)


def prepare(args):
    home = args.home.resolve()
    final_home = args.final_home.resolve() if getattr(args, "final_home", None) else home
    resources = args.package_root if getattr(args, "package_root", None) else home
    deploy = args.deploy_dir if getattr(args, "deploy_dir", None) else home / "bin"
    relocate_runtime(home, home, args.machine)
    if args.qt == "1":
        environment = read(args.qt_environment) if args.qt_environment else qt_environment(final_home)
        bridge = resources / ("build-aarch64/QtDisplayBridge" if resources != home else "bin/QtDisplayBridge")
        if not (resources / "ky-ems/KY-EMS").is_file() or not bridge.is_file():
            raise ValueError("local Qt needs KY-EMS and QtDisplayBridge")
        (home / "tmp").mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="generic-scada-", dir=str(home / "tmp")) as directory:
            package = Path(directory) / "runtime.kyscada"
            make_project(home, args.machine, package, resources)
            subprocess.run(["sh", str(deploy / "install-scada-project.sh"), "--package", str(package),
                            "--machine-code", args.machine, "--app-config", str(home / "config/runtime/apps/monitor-service.json"),
                            "--scada-root", str(home / "scada"), "--enable-native-display"], check=True)
        path = home / "config/runtime/qt-display.env"
        fd, temporary = tempfile.mkstemp(prefix=".qt-display-", dir=str(path.parent))
        try:
            with os.fdopen(fd, "w") as stream:
                for key in QT_KEYS:
                    if key in environment:
                        stream.write(key + "=" + environment[key] + "\n")
            os.replace(temporary, str(path))
        finally:
            if os.path.exists(temporary):
                os.unlink(temporary)
    if final_home != home:
        # The private stage is physical storage, never a runtime reference.
        relocate_runtime(home, final_home, args.machine, home, monitor_ascii=args.qt != "1")
        for path in (home / "scada/backup").glob("monitor-service-*.json"):
            write(path, relocate(read(path), final_home, args.machine, home))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command")
    check = commands.add_parser("preflight")
    check.add_argument("--package-root", required=True, type=Path)
    check.add_argument("--deploy-dir", required=True, type=Path)
    check.add_argument("--profile", required=True, choices=("base", "project", "full"))
    check.add_argument("--mode", required=True, choices=("gateway", "ems", "agc_avc"))
    check.add_argument("--manifest", default="")
    check.add_argument("--selection", required=True, type=Path)
    check.add_argument("--machine", required=True)
    check.add_argument("--qt", choices=("auto", "0", "1"), required=True)
    check.add_argument("--home", type=Path, required=True)
    check.add_argument("--start", required=True)
    activate = commands.add_parser("prepare")
    activate.add_argument("--home", required=True, type=Path)
    activate.add_argument("--machine", required=True)
    activate.add_argument("--qt", choices=("0", "1"), required=True)
    activate.add_argument("--qt-environment", default="")
    activate.add_argument("--final-home", type=Path)
    activate.add_argument("--package-root", type=Path)
    activate.add_argument("--deploy-dir", type=Path)
    widgets = commands.add_parser("install-widgets")
    widgets.add_argument("--package-root", required=True, type=Path)
    widgets.add_argument("--home", required=True, type=Path)
    tls = commands.add_parser("prepare-tls-apps")
    tls.add_argument("--app-config", required=True, type=Path)
    tls.add_argument("--ca-file", required=True)
    tls.add_argument("--cert-file", required=True)
    tls.add_argument("--key-file", required=True)
    tls.add_argument("--require-prepared", action="store_true")
    for command in ("check-kiosk-parents", "prepare-kiosk-parents", "project-kiosk", "install-kiosk-script"):
        kiosk = commands.add_parser(command)
        kiosk.add_argument("--home", required=True, type=Path)
        if command == "project-kiosk":
            kiosk.add_argument("--instance", required=True)
            kiosk.add_argument("--if-selected", action="store_true")
        if command == "install-kiosk-script":
            kiosk.add_argument("--source", required=True, type=Path)
    for command in ("check-kiosk-source", "check-kiosk-app"):
        check_source = commands.add_parser(command)
        check_source.add_argument("--source", required=True, type=Path)
    args = parser.parse_args()
    if args.command is None:
        parser.error("a command is required")
    try:
        if args.command == "preflight":
            preflight(args)
        elif args.command == "install-widgets":
            install_widgets(args)
        elif args.command == "prepare-tls-apps":
            prepare_tls_apps(args)
        elif args.command == "check-kiosk-parents":
            check_kiosk_parents(args)
        elif args.command == "check-kiosk-source":
            check_kiosk_source(args)
        elif args.command == "check-kiosk-app":
            check_kiosk_app(args)
        elif args.command == "prepare-kiosk-parents":
            prepare_kiosk_parents(args)
        elif args.command == "project-kiosk":
            project_kiosk(args)
        elif args.command == "install-kiosk-script":
            install_kiosk_script(args)
        else:
            prepare(args)
    except (ValueError, KeyError, OSError, subprocess.CalledProcessError) as error:
        if "kiosk" in args.command:
            category = str(error) if isinstance(error, ValueError) and str(error).startswith("KIOSK_") else "KIOSK_IO_FAILURE"
            parser.exit(2, category + "\n")
        parser.exit(2, str(error) + "\n")
