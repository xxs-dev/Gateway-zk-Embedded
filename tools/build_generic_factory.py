#!/usr/bin/env python3
"""Create a review-only generic candidate from an exactly verified programs archive."""
import argparse
import copy
import gzip
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import tarfile
import tempfile


PROGRAM_COMMIT = "5d40272e61e708e4694b4bdc6f15e2886efcd73e"
PROGRAM_SHA = "d7c6646370c884496b072a226de12889dea4931742cf867ffeb9cdb33c1bff09"
MANIFEST_SHA = "bdb874460cf7cfbb6c2d77514b5f21de515893ad01f4185fe778da4a499ad404"
SOURCE = Path(__file__).resolve().parents[1]


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def read(path):
    return json.loads(Path(path).read_text(encoding="utf-8"))


def write(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=True, indent=2) + "\n", encoding="utf-8")


def hashes(root):
    return {str(path.relative_to(root)).replace("\\", "/"): sha(path)
            for path in sorted(root.rglob("*")) if path.is_file()}


def source_git(*args):
    command = ["git", "-C", str(SOURCE)]
    reference = SOURCE / ".git"
    if reference.is_file():
        gitdir = reference.read_text().strip().removeprefix("gitdir: ").replace("\\", "/")
        if str(SOURCE).startswith("/mnt/") and re.match(r"^[A-Za-z]:/", gitdir):
            gitdir = "/mnt/" + gitdir[0].lower() + gitdir[2:]
            command = ["git", "--git-dir=" + gitdir, "--work-tree=" + str(SOURCE)]
    return subprocess.check_output([*command, *args], text=True).strip()


def archive(root, destination):
    with destination.open("wb") as raw, gzip.GzipFile(fileobj=raw, mode="wb", filename="", mtime=0) as compressed:
        with tarfile.open(fileobj=compressed, mode="w") as output:
            for path in [root] + sorted(root.rglob("*")):
                name = str(Path(root.name) / path.relative_to(root)).replace("\\", "/")
                info = output.gettarinfo(str(path), arcname=name)
                info.mtime, info.uid, info.gid, info.uname, info.gname = 0, 0, 0, "", ""
                info.mode = 0o755 if info.isdir() or path.suffix in (".sh", ".py") or path.parent.name in ("build-aarch64", "ky-ems") else 0o644
                if info.isfile():
                    with path.open("rb") as data:
                        output.addfile(info, data)
                else:
                    output.addfile(info)


def sanitize(value):
    if isinstance(value, dict):
        result = {}
        for key, item in value.items():
            if key.lower() in ("password", "secretkey", "accesskey", "token", "apikey", "username", "imei", "serialnumber"):
                result[key] = ""
            elif key == "machineCode" or key == "clientId":
                result[key] = "GW_FACTORY_001"
            else:
                result[key] = sanitize(item)
        return result
    if isinstance(value, list):
        return [sanitize(item) for item in value]
    if isinstance(value, str):
        value = re.sub(r"COMM\d{6,}", "GW_FACTORY_001", value)
        return value
    return value


def stage_config(stage):
    runtime = stage / "config/factory/runtime"
    for path in (SOURCE / "config/factory/runtime/apps").glob("*.json"):
        data = sanitize(read(path))
        if path.name != "agc-avc-service.json":
            data["runtimeMode"] = "gateway"
        data["deviceConfigFiles"] = []
        if "localDisplay" in data:
            data["localDisplay"]["enabled"] = False
            data["localDisplay"]["renderer"] = "nativeQt"
            data["localDisplay"]["screens"] = []
            data["localDisplay"].setdefault("scada", {})["enabled"] = False
        for key in ("ota", "computeEngine", "eventEngine", "emsCluster", "cameraService"):
            if isinstance(data.get(key), dict):
                data[key]["enabled"] = False
        if "computeEngine" in data:
            data["computeEngine"]["rules"] = []
        if "cameraService" in data:
            data["cameraService"]["cameras"] = []
        monitor = data.get("systemMonitor", {})
        if "cellular" in monitor:
            monitor["cellular"]["enabled"] = False
            monitor["cellular"].setdefault("routeFailover", {})["enabled"] = False
        if "agcAvc" in data:
            data["agcAvc"]["enabled"] = False
            data["agcAvc"]["shadowMode"] = True
            data["agcAvc"]["submitWrites"] = False
        write(runtime / "apps" / path.name, data)
    write(runtime / "device_identity.json", sanitize(read(SOURCE / "config/factory/runtime/device_identity.json")))
    (runtime / "devices").mkdir(parents=True)
    # No site point table is a valid default for an uncommissioned gateway.
    # Keep a disabled generic virtual config for the separately selected AGC mode.
    write(runtime / "devices/device_agc_avc_virtual.json", {
        "schemaVersion": "1.1.0", "enabled": False,
        "protocol": {"type": "virtual"}, "meters": [],
        "memoryStore": {"enabled": True, "backend": "memory", "sharedMemoryName": "gateway_point_store_agc_avc"}})
    app = read(runtime / "apps/agc-avc-service.json")
    app["deviceConfigFiles"] = ["/opt/modbus-gateway/config/runtime/devices/device_agc_avc_virtual.json"]
    write(runtime / "apps/agc-avc-service.json", app)
    # Widget prototypes retain the product rendering contract, not its site bindings.
    screen = read(SOURCE / "products/ems/mobile/2.0/scada/base-project/screens/Devices.json")
    prototypes = {}
    for kind in ("qtLabel", "qtValue", "qtButton"):
        widget = copy.deepcopy(next(item for item in screen["widgets"] if item["type"] == kind))
        widget.update(widgetId="generic", title="", styleClass="", bindings=[], stateRules=[], action=None)
        widget["properties"] = {"qtText": "", "qtTextColor": "#17212B", "qtBackgroundColor": "transparent",
                                "qtFontSize": 18, "qtTextAlignment": "Left", "qtTransparent": True}
        prototypes[kind] = widget
    write(stage / "config/generic-scada-widgets.json", prototypes)


def main(args):
    if sha(args.programs) != PROGRAM_SHA or sha(args.manifest) != MANIFEST_SHA:
        raise ValueError("fixed program archive/manifest SHA mismatch")
    manifest = read(args.manifest)
    if manifest["sourceCommit"] != PROGRAM_COMMIT or manifest["sourceDirty"]:
        raise ValueError("unexpected binary source provenance")
    previous = None
    if args.reuse_agc_from:
        previous = read(args.reuse_agc_from / "component-provenance.json")
        if (previous["binarySourceCommit"] != PROGRAM_COMMIT or previous["programArchiveSha256"] != PROGRAM_SHA or
                previous["programManifestSha256"] != MANIFEST_SHA):
            raise ValueError("reused AGC has different binary provenance")
        for name, metadata in previous["artifacts"].items():
            if sha(args.reuse_agc_from / name) != metadata["sha256"]:
                raise ValueError("previous candidate artifact changed: " + name)
    if args.candidate_id and not re.fullmatch(r"candidate-[0-9]+", args.candidate_id):
        raise ValueError("candidate id must have the form candidate-NN")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    source_commit = source_git("rev-parse", "HEAD")
    source_status = source_git("status", "--porcelain")
    inputs = {str(path.relative_to(SOURCE)): sha(path) for directory in ("deploy", "config/factory")
              for path in sorted((SOURCE / directory).rglob("*")) if path.is_file()}
    inputs["tools/build_generic_factory.py"] = sha(Path(__file__))
    widget_path = "products/ems/mobile/2.0/scada/base-project/screens/Devices.json"
    inputs[widget_path] = sha(SOURCE / widget_path)
    with tempfile.TemporaryDirectory(prefix="generic-factory-") as temporary:
        stage = Path(temporary) / "source"
        stage.mkdir()
        (stage / "deploy").mkdir()
        for path in (SOURCE / "deploy").iterdir():
            if path.is_file() and path.suffix in (".sh", ".py", ".service", ".default", ".conf"):
                (stage / "deploy" / path.name).write_bytes(path.read_bytes().replace(b"\r\n", b"\n"))
        stage_config(stage)
        components = []
        with tarfile.open(str(args.programs), "r:gz") as package:
            members = {member.name: member for member in package.getmembers()}
            if len(members) != len(package.getmembers()):
                raise ValueError("duplicate program archive members")
            for member in members.values():
                if member.issym() or member.islnk() or member.name.startswith("/") or ".." in PurePosixPath(member.name).parts:
                    raise ValueError("unsafe program archive member")
            for component in manifest["components"]:
                member = members[component["archivePath"]]
                data = package.extractfile(member).read()
                if len(data) != component["sizeBytes"] or hashlib.sha256(data).hexdigest() != component["sha256"]:
                    raise ValueError("program component hash mismatch: " + component["binary"])
                if data[:4] != b"\x7fELF" or int.from_bytes(data[18:20], "little") != 183:
                    raise ValueError("program is not AArch64: " + component["binary"])
                relative = "ky-ems/KY-EMS" if component["binary"] == "KY-EMS" else "build-aarch64/" + component["binary"]
                destination = stage / relative
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_bytes(data)
                destination.chmod(0o755)
                components.append({"binary": component["binary"], "path": relative,
                                   "sizeBytes": len(data), "sha256": component["sha256"],
                                   "sourceCommit": PROGRAM_COMMIT, "sourceArchiveSha256": PROGRAM_SHA})
        factory_raw = Path(temporary) / "factory.tar.gz"
        overlay_raw = Path(temporary) / "overlay.tar.gz"
        for script, target in (("build-factory-package.sh", factory_raw), ("build-agc-avc-runtime-package.sh", overlay_raw)):
            process = subprocess.run(["sh", str(stage / "deploy" / script), str(target)],
                                     text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            (output / (script + ".log")).write_text(process.stdout)
            if process.returncode:
                raise RuntimeError("package staging failed: " + script)
        roots = []
        for name, packed in (("factory", factory_raw), ("agc", overlay_raw)):
            parent = Path(temporary) / name
            parent.mkdir()
            with tarfile.open(str(packed), "r:gz") as package:
                # This archive was just created by the repository's own builder.
                package.extractall(str(parent), filter="data")
            roots.append(parent / "gateway-factory-defaults")
        factory, overlay = roots
        shutil.copyfile(stage / "config/generic-scada-widgets.json", factory / "config/generic-scada-widgets.json")
        agc_components = [item for item in components if item["binary"] == "AgcAvcController"]
        agc_manifest = read(overlay / "agc-avc-runtime-manifest.json")
        agc_manifest.update(sourceCommit=PROGRAM_COMMIT, binarySourceCommit=PROGRAM_COMMIT,
                            assemblySourceCommit=source_commit, assemblySourceDirty=bool(source_status),
                            sourceFileHashes=inputs, components=agc_components, files=hashes(overlay))
        agc_manifest["files"].pop("agc-avc-runtime-manifest.json", None)
        write(overlay / "agc-avc-runtime-manifest.json", agc_manifest)
        package_manifest = read(factory / "edge-package-manifest.json")
        package_manifest.update(sourceCommit=source_commit, sourceDirty=bool(source_status),
                                initializationKind="generic-uncommissioned",
                                packageVersion="generic-review-20260917" + ("-" + args.candidate_id if args.candidate_id else ""),
                                binarySourceCommit=PROGRAM_COMMIT,
                                assemblySourceCommit=source_commit, assemblySourceDirty=bool(source_status),
                                createdBy="build_generic_factory.py", deploymentApproved=False)
        for component in package_manifest["components"]:
            component["sourceCommit"] = PROGRAM_COMMIT
            component["sourceArchiveSha256"] = PROGRAM_SHA
        write(factory / "edge-package-manifest.json", package_manifest)
        write(factory / "generic-package.json", {"schemaVersion": 1, "kind": "generic-review-candidate",
              "initializationKind": "generic-uncommissioned",
              "binarySourceCommit": PROGRAM_COMMIT, "assemblySourceCommit": source_commit,
              "assemblySourceDirty": bool(source_status), "deploymentApproved": False,
              "hardwareAcceptance": "NOT_RUN", "files": hashes(factory)})
        archive(factory, output / "gateway-factory-defaults.tar.gz")
        overlay_files = hashes(overlay)
        if previous:
            if components != previous["components"]:
                raise ValueError("previous candidate program component metadata changed")
            old_archive = args.reuse_agc_from / "gateway-agc-avc-runtime.tar.gz"
            with tarfile.open(old_archive) as package:
                files = {}
                for member in package.getmembers():
                    if member.issym() or member.islnk() or ".." in PurePosixPath(member.name).parts:
                        raise ValueError("unsafe reused AGC archive member")
                    if not member.isfile():
                        continue
                    name = member.name.removeprefix("gateway-factory-defaults/")
                    if name in files:
                        raise ValueError("duplicate reused AGC archive member")
                    files[name] = hashlib.sha256(package.extractfile(member).read()).hexdigest()
                if files != previous["overlayFiles"]:
                    raise ValueError("reused AGC file hashes differ from provenance")
            def payload(files):
                return {name: digest for name, digest in files.items() if name != "agc-avc-runtime-manifest.json"}
            if payload(files) != payload(overlay_files):
                raise ValueError("AGC payload changed; exact archive reuse is not possible")
            shutil.copyfile(old_archive, output / "gateway-agc-avc-runtime.tar.gz")
            overlay_files = files
        else:
            archive(overlay, output / "gateway-agc-avc-runtime.tar.gz")
        shutil.copytree(factory / "deploy", output / "deploy")
        # The optional AGC service comes from the same overlay and script source.
        shutil.copyfile(overlay / "deploy/agc-avc@.service", output / "deploy/agc-avc@.service")
        inheritance = None
        if previous:
            def changes(old, new):
                return {name: {"before": old.get(name), "after": new.get(name)} for name in sorted(set(old) | set(new))
                        if old.get(name) != new.get(name)}
            inheritance = {"provenanceSha256": sha(args.reuse_agc_from / "component-provenance.json"),
                           "artifacts": previous["artifacts"], "agcArchiveReusedByteForByte": True,
                           "agcManifestRetainsOriginalAssemblyProvenance": True,
                           "programComponentsUnchanged": True,
                           "factoryChanges": changes(previous["factoryFiles"], hashes(factory)),
                           "pairedDeployChanges": changes(previous["pairedDeployFiles"], hashes(output / "deploy"))}
        write(output / "component-provenance.json", {"binarySourceCommit": PROGRAM_COMMIT,
              "candidateId": args.candidate_id, "inheritedCandidate": inheritance,
              "programArchiveSha256": PROGRAM_SHA, "programManifestSha256": MANIFEST_SHA,
              "assemblySourceCommit": source_commit, "assemblySourceDirty": bool(source_status),
              "sourceFileHashes": inputs, "components": components,
              "factoryFiles": hashes(factory), "overlayFiles": overlay_files, "pairedDeployFiles": hashes(output / "deploy"),
              "artifacts": {name: {"sha256": sha(output / name), "sizeBytes": (output / name).stat().st_size}
                            for name in ("gateway-factory-defaults.tar.gz", "gateway-agc-avc-runtime.tar.gz")},
              "deploymentApproved": False, "hardwareAcceptance": "NOT_RUN"})
    print(str(output / "component-provenance.json"))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--programs", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--candidate-id", default="")
    parser.add_argument("--reuse-agc-from", type=Path,
                        help="Reuse a verified previous candidate AGC archive unchanged, including its original provenance")
    main(parser.parse_args())
