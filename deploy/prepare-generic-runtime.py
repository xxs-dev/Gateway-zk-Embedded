#!/usr/bin/env python3
"""Verify a generic bundle and prepare a read-only, identity-bound local display."""
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import zipfile
import zlib


def read(path):
    return json.loads(Path(path).read_text(encoding="utf-8"))


def write(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=".generic-", dir=str(path.parent))
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as output:
            json.dump(value, output, ensure_ascii=True, indent=2)
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


def relocate(value, home, machine):
    if isinstance(value, dict):
        return {key: (machine if key == "machineCode" else relocate(item, home, machine))
                for key, item in value.items()}
    if isinstance(value, list):
        return [relocate(item, home, machine) for item in value]
    if isinstance(value, str) and value.startswith("/opt/modbus-gateway/"):
        return str(home) + value[len("/opt/modbus-gateway"):]
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


def make_project(home, machine, output):
    tags, routes = inventory(home, machine)
    # Reuse the product Qt widget schema, with no inherited site tags or controls.
    prototypes = read(home / "config/generic-scada-widgets.json")
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


def prepare(args):
    home = args.home.resolve()
    for path in (home / "config/runtime").rglob("*.json"):
        write(path, relocate(read(path), home, args.machine))
    if args.qt == "0":
        return
    environment = read(args.qt_environment) if args.qt_environment else qt_environment(home)
    if not (home / "ky-ems/KY-EMS").is_file() or not (home / "bin/QtDisplayBridge").is_file():
        raise ValueError("local Qt needs KY-EMS and QtDisplayBridge")
    with tempfile.TemporaryDirectory(prefix="generic-scada-", dir=str(home / "tmp")) as directory:
        package = Path(directory) / "runtime.kyscada"
        make_project(home, args.machine, package)
        subprocess.run(["sh", str(home / "bin/install-scada-project.sh"), "--package", str(package),
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
    args = parser.parse_args()
    if args.command is None:
        parser.error("a command is required")
    try:
        if args.command == "preflight":
            preflight(args)
        else:
            prepare(args)
    except (ValueError, KeyError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(2, str(error) + "\n")
