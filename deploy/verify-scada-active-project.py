#!/usr/bin/env python3
"""Verify the active SCADA release against the factory package contract."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import re
import stat
import sys
import tempfile
from typing import Any


ALGORITHM = "sorted-jsonl-tree-v1"
DIRECTORY_SIZE_POLICY = "fixed-4096"
NORMALIZED_DIRECTORY_SIZE = 4096
SHA256_PATTERN = re.compile(r"[0-9a-f]{64}")
REQUIRED_DOCUMENTS = {
    "manifest.json",
    "topology.json",
    "nodes.json",
    "tags.json",
    "runtime-map.json",
    "checksums.json",
}


class VerificationError(ValueError):
    pass


def sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def stable_json_line(value: dict[str, Any]) -> str:
    return json.dumps(value, ensure_ascii=True, sort_keys=True, separators=(",", ":"))


def reject_duplicate_keys(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise VerificationError(f"duplicate-json-key:{key}")
        result[key] = value
    return result


def load_json(path: pathlib.Path, label: str) -> Any:
    try:
        return json.loads(
            path.read_text(encoding="utf-8"),
            object_pairs_hook=reject_duplicate_keys,
        )
    except VerificationError:
        raise
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise VerificationError(f"invalid-json:{label}") from exc


def require_sha256(value: Any, label: str) -> str:
    normalized = str(value or "").strip().lower()
    if not SHA256_PATTERN.fullmatch(normalized):
        raise VerificationError(f"invalid-expected-sha256:{label}")
    return normalized


def is_within(path: pathlib.Path, root: pathlib.Path) -> bool:
    try:
        path.relative_to(root)
        return True
    except ValueError:
        return False


def parse_install_state(path: pathlib.Path) -> dict[str, str]:
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as exc:
        raise VerificationError("install-state-missing") from exc
    state: dict[str, str] = {}
    for line in lines:
        if not line or "=" not in line:
            continue
        key, value = line.split("=", 1)
        if key in state:
            raise VerificationError(f"install-state-duplicate-key:{key}")
        state[key] = value
    return state


def inspect_tree(root: pathlib.Path) -> dict[str, Any]:
    canonical_lines: list[str] = []
    content_lines: list[str] = []
    regular_hashes: dict[str, str] = {}
    counts = {"files": 0, "directories": 0, "bytes": 0}

    def visit(path: pathlib.Path, relative: str) -> None:
        try:
            info = os.lstat(path)
        except OSError as exc:
            raise VerificationError(f"tree-entry-unreadable:{relative}") from exc
        if stat.S_ISLNK(info.st_mode):
            raise VerificationError(f"tree-symlink-present:{relative}")
        if stat.S_ISDIR(info.st_mode):
            kind = "directory"
            entry_size = NORMALIZED_DIRECTORY_SIZE
            counts["directories"] += 1
        elif stat.S_ISREG(info.st_mode):
            kind = "file"
            entry_size = info.st_size
            counts["files"] += 1
            counts["bytes"] += info.st_size
        else:
            raise VerificationError(f"tree-special-file-present:{relative}")

        canonical: dict[str, Any] = {
            "path": relative,
            "mode": format(stat.S_IMODE(info.st_mode), "04o"),
            "uid": info.st_uid,
            "gid": info.st_gid,
            "size": entry_size,
            "type": kind,
        }
        content: dict[str, Any] = {
            "path": relative,
            "size": entry_size,
            "type": kind,
        }
        if kind == "file":
            file_sha256 = sha256_file(path)
            canonical["sha256"] = file_sha256
            content["sha256"] = file_sha256
            regular_hashes[relative] = file_sha256
        canonical_lines.append(stable_json_line(canonical))
        content_lines.append(stable_json_line(content))

        if kind == "directory":
            try:
                children = sorted(os.scandir(path), key=lambda item: item.name)
            except OSError as exc:
                raise VerificationError(f"tree-directory-unreadable:{relative}") from exc
            for child in children:
                child_relative = child.name if relative == "." else f"{relative}/{child.name}"
                visit(pathlib.Path(child.path), child_relative)

    visit(root, ".")
    canonical_bytes = ("\n".join(canonical_lines) + "\n").encode("utf-8")
    content_bytes = ("\n".join(content_lines) + "\n").encode("utf-8")
    return {
        "canonicalManifestSha256": hashlib.sha256(canonical_bytes).hexdigest(),
        "contentManifestSha256": hashlib.sha256(content_bytes).hexdigest(),
        "fileCount": counts["files"],
        "directoryCount": counts["directories"],
        "totalRegularFileBytes": counts["bytes"],
        "regularFileHashes": regular_hashes,
    }


def validate_project_metadata(
    active: pathlib.Path,
    machine_code: str,
    regular_hashes: dict[str, str],
) -> dict[str, Any]:
    if not REQUIRED_DOCUMENTS.issubset(regular_hashes):
        missing = sorted(REQUIRED_DOCUMENTS - regular_hashes.keys())
        raise VerificationError("required-document-missing:" + ",".join(missing))
    project = load_json(active / "manifest.json", "manifest.json")
    topology = load_json(active / "topology.json", "topology.json")
    nodes = load_json(active / "nodes.json", "nodes.json")
    checksums = load_json(active / "checksums.json", "checksums.json")
    if not isinstance(project, dict) or project.get("schemaVersion") != "2.0":
        raise VerificationError("unsupported-scada-schema")
    project_id = str(project.get("projectId") or "").strip()
    version = str(project.get("packageVersion") or "").strip()
    if not project_id or not version:
        raise VerificationError("project-metadata-missing")
    if not isinstance(nodes, list):
        raise VerificationError("nodes-not-array")
    matches = [node for node in nodes if isinstance(node, dict) and node.get("machineCode") == machine_code]
    if len(matches) != 1:
        raise VerificationError("machine-code-match-count-invalid")
    node_id = str(matches[0].get("nodeId") or "").strip()
    if not node_id:
        raise VerificationError("node-id-missing")
    local_scada = (
        isinstance(topology, dict)
        and topology.get("mode", "integrated") == "integrated"
        and project.get("packageRole", "project") != "edgeNode"
    )
    if not local_scada:
        raise VerificationError("local-scada-required")
    if not isinstance(checksums, dict):
        raise VerificationError("checksums-not-object")
    expected_files = set(regular_hashes) - {"checksums.json"}
    if set(checksums) != expected_files:
        raise VerificationError("checksum-file-set-mismatch")
    for relative, expected in checksums.items():
        if not isinstance(relative, str):
            raise VerificationError("checksum-path-invalid")
        path = pathlib.PurePosixPath(relative)
        if path.is_absolute() or any(part in ("", ".", "..") for part in path.parts):
            raise VerificationError("checksum-path-unsafe")
        expected_sha256 = require_sha256(expected, f"checksum:{relative}")
        if regular_hashes.get(relative) != expected_sha256:
            raise VerificationError(f"checksum-mismatch:{relative}")
    return {
        "projectId": project_id,
        "version": version,
        "machineCode": machine_code,
        "nodeId": node_id,
        "localScada": True,
        "checksumCount": len(checksums),
    }


def write_result(path: pathlib.Path, result: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as output:
            json.dump(result, output, ensure_ascii=True, indent=2, sort_keys=True)
            output.write("\n")
            output.flush()
            os.fsync(output.fileno())
        os.chmod(temporary, 0o644)
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", required=True, type=pathlib.Path)
    parser.add_argument("--current", required=True, type=pathlib.Path)
    parser.add_argument("--releases-root", required=True, type=pathlib.Path)
    parser.add_argument("--install-state", required=True, type=pathlib.Path)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    parser.add_argument("--allow-missing-current", action="store_true")
    parser.add_argument("--require-scada-contract", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    source_mode = "unknown"
    current_exists = args.current.exists()
    result: dict[str, Any] = {
        "schemaVersion": "1.0",
        "algorithm": ALGORITHM,
        "directorySizePolicy": DIRECTORY_SIZE_POLICY,
        "status": "blocked",
        "sourceMode": source_mode,
        "currentExists": current_exists,
        "requiredBeforeServiceStart": True,
        "findings": [],
    }
    try:
        manifest = load_json(args.manifest, "edge-package-manifest.json")
        if not isinstance(manifest, dict):
            raise VerificationError("package-manifest-not-object")
        schema = str(manifest.get("schemaVersion") or "").strip()
        if not schema.startswith(("1.1", "1.2")):
            raise VerificationError("package-manifest-schema-unsupported")
        components = manifest.get("components")
        if not isinstance(components, list) or not components:
            raise VerificationError("package-manifest-components-invalid")
        if any(not isinstance(component, dict) for component in components):
            raise VerificationError("package-manifest-component-invalid")
        packaged_names = {
            str(component.get("binary") or component.get("name") or "").strip()
            for component in components
        }
        scada = manifest.get("scadaProject")
        if not isinstance(scada, dict):
            if args.require_scada_contract or "KY-EMS" in packaged_names:
                raise VerificationError("ky-ems-scada-contract-missing")
            result.update({"status": "not-required", "requiredBeforeServiceStart": False})
            write_result(args.output, result)
            return 0
        source_mode = str(scada.get("sourceMode") or "embedded-package").strip()
        result["sourceMode"] = source_mode
        if source_mode not in ("embedded-package", "external-active-project"):
            raise VerificationError("source-mode-invalid")
        if scada.get("required") is not True:
            raise VerificationError("scada-not-required")
        if str(scada.get("canonicalAlgorithm") or "") != ALGORITHM:
            raise VerificationError("canonical-algorithm-mismatch")
        if str(scada.get("directorySizePolicy") or "") != DIRECTORY_SIZE_POLICY:
            raise VerificationError("directory-size-policy-mismatch")
        expected_package = require_sha256(scada.get("sha256"), "package")
        expected_canonical = require_sha256(scada.get("canonicalManifestSha256"), "canonical")
        expected_content = require_sha256(scada.get("contentManifestSha256"), "content")
        machine_code = str(scada.get("machineCode") or "").strip()
        if not machine_code:
            raise VerificationError("machine-code-missing")
        result["expected"] = {
            "packageSha256": expected_package,
            "canonicalManifestSha256": expected_canonical,
            "contentManifestSha256": expected_content,
            "machineCode": machine_code,
        }

        current_exists = args.current.exists()
        result["currentExists"] = current_exists
        if not current_exists:
            if args.allow_missing_current and source_mode == "external-active-project":
                result.update({"status": "pending", "findings": ["current-missing"]})
                write_result(args.output, result)
                return 0
            raise VerificationError("current-missing")
        if not args.current.is_symlink():
            raise VerificationError("current-not-symlink")
        active = args.current.resolve(strict=True)
        releases_root = args.releases_root.resolve(strict=True)
        if not active.is_dir() or not is_within(active, releases_root):
            raise VerificationError("current-target-outside-releases")

        snapshot = inspect_tree(active)
        if snapshot["canonicalManifestSha256"] != expected_canonical:
            raise VerificationError("canonical-sha256-mismatch")
        if snapshot["contentManifestSha256"] != expected_content:
            raise VerificationError("content-sha256-mismatch")
        metadata = validate_project_metadata(active, machine_code, snapshot["regularFileHashes"])

        state = parse_install_state(args.install_state)
        state_expectations = {
            "scadaStatus": "ready",
            "scadaPackageSha256": expected_package,
            "scadaCanonicalManifestSha256": expected_canonical,
            "scadaContentManifestSha256": expected_content,
            "scadaMachineCode": machine_code,
            "scadaProjectId": metadata["projectId"],
            "scadaVersion": metadata["version"],
            "scadaNodeId": metadata["nodeId"],
            "scadaLocalScada": "true",
            "scadaAlgorithm": ALGORITHM,
            "scadaDirectorySizePolicy": DIRECTORY_SIZE_POLICY,
        }
        for key, expected in state_expectations.items():
            if state.get(key) != expected:
                raise VerificationError(f"install-state-mismatch:{key}")
        try:
            state_target = pathlib.Path(state["scadaCurrentTarget"]).resolve(strict=True)
        except (KeyError, OSError) as exc:
            raise VerificationError("install-state-current-target-invalid") from exc
        if state_target != active:
            raise VerificationError("install-state-current-target-mismatch")

        result.update({
            "status": "ready",
            "activeRelease": active.name,
            "actual": {
                "canonicalManifestSha256": snapshot["canonicalManifestSha256"],
                "contentManifestSha256": snapshot["contentManifestSha256"],
                "fileCount": snapshot["fileCount"],
                "directoryCount": snapshot["directoryCount"],
                "totalRegularFileBytes": snapshot["totalRegularFileBytes"],
                **metadata,
            },
            "findings": [],
        })
        write_result(args.output, result)
        print(
            "SCADA active runtime ready: "
            f"canonical={expected_canonical} content={expected_content} machineCode={machine_code}"
        )
        return 0
    except (VerificationError, OSError) as exc:
        result.update({"status": "blocked", "sourceMode": source_mode, "currentExists": current_exists})
        result["findings"] = [str(exc)]
        write_result(args.output, result)
        print(f"SCADA active runtime blocked: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
