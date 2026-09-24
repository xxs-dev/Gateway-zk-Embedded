#!/usr/bin/env python3
"""Read-only deployment gates. No ABI migration or control approval is implicit."""
import json
import os
from pathlib import Path
import struct
import sys

COMPATIBILITY = {"pointStoreAbi": 11, "clusterProtocol": 2,
                 "upgradeMode": "offline-all-participants"}
RUNTIME_BINARIES = frozenset((
    "ModbusRtu", "Dlt645Driver", "DioDriver", "CanDriver", "IecDriver", "MqttDriver",
    "MqttForwarder", "EventEngine", "EventStore", "ComputeEngine", "AgcAvcController",
    "EmsParityCheck", "EmsClusterCoordinator", "SystemMonitor", "LocalDisplay", "LocalDisplayQtEms",
    "QtDisplayBridge", "CameraService", "pointctl", "KY-EMS", "memory_point_store_migrate"))


def compatibility(document):
    value = document.get("runtimeCompatibility")
    if value is None and "runtimeCompatibility" not in document:
        return None
    if not isinstance(value, dict) or set(value) != set(COMPATIBILITY):
        raise ValueError("runtimeCompatibility requires exactly pointStoreAbi, clusterProtocol, upgradeMode")
    for key, expected in COMPATIBILITY.items():
        if type(value[key]) is not type(expected) or value[key] != expected:
            raise ValueError("unsupported or mistyped runtimeCompatibility." + key)
    return value


def elf(path):
    if not path.is_file():
        return False
    with path.open('rb') as stream:
        return stream.read(4) == b'\x7fELF'


def runtime_file(source, destination):
    return destination.name in RUNTIME_BINARIES or elf(source) or elf(destination)


def ota(manifest_path):
    if Path('/opt/modbus-gateway/data/runtime-upgrade-stop').exists():
        raise ValueError('offline upgrade fence active; ordinary OTA cannot change this runtime')
    manifest = json.loads(manifest_path.read_text())
    if not isinstance(manifest, dict):
        raise ValueError("manifest must be an object")
    compatibility(manifest)
    files = manifest.get('files')
    if not isinstance(files, list) or not files or any(not isinstance(item, dict) for item in files):
        raise ValueError("manifest files must be a non-empty object array")
    for item in files:
        source = (manifest_path.parent / str(item.get('path', ''))).resolve()
        if manifest_path.parent.resolve() not in source.parents or not source.is_file():
            raise ValueError("unsafe or missing OTA source")
        destination = Path(str(item.get('target', '')))
        if runtime_file(source, destination):
            raise ValueError("runtime ELF OTA requires the offline all-participant upgrade; declarations alone do not prove ABI compatibility")


def rollback(backup):
    if Path('/opt/modbus-gateway/data/runtime-upgrade-stop').exists():
        raise ValueError('offline upgrade fence active; use offline recovery')
    # Ordinary OTA cannot prove the old/new participant ABI from a backup filename.
    for top in ('opt', 'etc'):
        root = backup / top
        if not root.exists():
            continue
        for source in root.rglob('*'):
            if source.is_symlink():
                raise ValueError("symlink in rollback backup")
            if source.is_file() and runtime_file(source, Path('/') / source.relative_to(backup)):
                raise ValueError("runtime ELF rollback requires offline all-participant recovery; keep control stopped")


def factory(home):
    for path in (home / 'config/runtime', home / 'data', home / 'bin'):
        if path.exists() and any(path.iterdir()):
            raise ValueError("factory initialization is not an in-place runtime upgrade: " + str(path))
    for segment in Path('/dev/shm').iterdir():
        if segment.is_symlink() or not segment.is_file():
            continue
        with segment.open('rb') as stream:
            if stream.read(4) == struct.pack('<I', 0x4d505354):
                raise ValueError("cold initialization requires no existing point-store segments: " + segment.name)
    for entry in Path('/proc').iterdir():
        if not entry.name.isdigit():
            continue
        try:
            executable = os.readlink(str(entry / 'exe'))
            if executable.endswith(' (deleted)'):
                executable = executable[:-10]
        except FileNotFoundError:
            continue
        except PermissionError:
            raise ValueError("cannot verify absence of old runtime processes")
        if Path(executable).name in RUNTIME_BINARIES:
            raise ValueError("old runtime participant is still running: " + entry.name)


def configured_names(value):
    names = set()
    if isinstance(value, dict):
        for key, item in value.items():
            if key in ('sharedMemoryName', 'virtualSharedMemoryName', 'outputSharedMemoryName') and isinstance(item, str) and item:
                names.add(item.lstrip('/'))
            elif key == 'sharedMemoryNames' and isinstance(item, list):
                names.update(str(name).lstrip('/') for name in item)
            else:
                names.update(configured_names(item))
    elif isinstance(value, list):
        for item in value:
            names.update(configured_names(item))
    return names


def startup(home):
    if (home / 'data/runtime-upgrade-stop').exists():
        raise ValueError('persistent offline upgrade fence active; no ordinary service start')
    names = {'gateway_point_store'}
    for config in (home / 'config/runtime').rglob('*.json'):
        names.update(configured_names(json.loads(config.read_text())))
    for name in names:
        if not name or any(ch not in 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-' for ch in name):
            raise ValueError("unsafe point-store name")
        path = Path('/dev/shm') / name
        if not path.exists():
            continue
        if path.is_symlink() or not path.is_file():
            raise ValueError("point-store must be a regular non-symlink segment: " + name)
        with path.open('rb') as stream:
            header = stream.read(8)
        if header != struct.pack('<II', 0x4d505354, 11):
            raise ValueError("point-store ABI mismatch; preserve it and use offline migration: " + name)


def main():
    if len(sys.argv) != 3 or sys.argv[1] not in ('ota', 'rollback', 'factory', 'startup'):
        raise ValueError("usage: runtime-upgrade-guard.py ota|rollback|factory|startup PATH")
    globals()[sys.argv[1]](Path(sys.argv[2]))


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, TypeError) as error:
        print('runtime upgrade refused: ' + str(error), file=sys.stderr)
        sys.exit(2)
