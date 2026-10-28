#!/usr/bin/env python3
"""Reject credential material from generic release directories and tar archives."""

from __future__ import print_function

import argparse
import collections
import io
import json
import os
import re
import sys
import tarfile

try:
    from urllib.parse import parse_qsl, urlsplit
except ImportError:  # pragma: no cover - Python 2 is unsupported, kept for tooling imports.
    from urlparse import parse_qsl, urlsplit


Finding = collections.namedtuple("Finding", "source location reason")
AuditResult = collections.namedtuple("AuditResult", "findings errors scanned_files")

MAX_AUDIT_FILE_BYTES = 128 * 1024 * 1024
MAX_AUDIT_ARCHIVE_DEPTH = 3
TAR_EXTENSIONS = (".tar", ".tar.gz", ".tgz", ".tar.bz2", ".tbz2", ".tar.xz", ".txz")
PRIVATE_KEY_MARKERS = (
    b"-----BEGIN PRIVATE KEY-----",
    b"-----BEGIN ENCRYPTED PRIVATE KEY-----",
    b"-----BEGIN RSA PRIVATE KEY-----",
    b"-----BEGIN EC PRIVATE KEY-----",
    b"-----BEGIN OPENSSH PRIVATE KEY-----",
)
SENSITIVE_NAMES = frozenset(
    (
        "password",
        "passwd",
        "accesskey",
        "secretkey",
        "apikey",
        "token",
        "authtoken",
        "clientsecret",
        "privatekey",
        "operatorcode",
    )
)
TEXT_CONFIG_EXTENSIONS = frozenset((".cfg", ".conf", ".env", ".ini", ".properties", ".yaml", ".yml"))
ASSIGNMENT_RE = re.compile(r"^\s*[\"']?([A-Za-z][A-Za-z0-9_.-]*)[\"']?\s*[:=]\s*(.*?)\s*$")


def _normalized_name(value):
    return "".join(ch.lower() for ch in str(value) if ch.isalnum())


def _is_sensitive_name(value):
    normalized = _normalized_name(value)
    if normalized in SENSITIVE_NAMES:
        return True
    if normalized.endswith("token") and not normalized.endswith("tokenparam"):
        return True
    return any(
        normalized.endswith(suffix)
        for suffix in ("password", "accesskey", "secretkey", "apikey", "authtoken")
    )


def _is_non_empty(value):
    if value is None:
        return False
    if isinstance(value, str):
        return bool(value.strip())
    if isinstance(value, (list, dict, tuple)):
        return bool(value)
    return True


def _json_pointer_token(value):
    return str(value).replace("~", "~0").replace("/", "~1")


def _audit_embedded_url(value, source, pointer, findings):
    if not isinstance(value, str) or ("://" not in value and "?" not in value):
        return
    try:
        parsed = urlsplit(value)
        if parsed.username or parsed.password:
            findings.append(Finding(source, pointer, "embedded URL contains userinfo credentials"))
        for key, query_value in parse_qsl(parsed.query, keep_blank_values=True):
            if _is_sensitive_name(key) and query_value.strip():
                findings.append(Finding(source, pointer, "embedded URL contains a non-empty sensitive query field"))
    except (TypeError, ValueError):
        return


def _audit_json(value, source, pointer, findings):
    if isinstance(value, dict):
        for key in sorted(value):
            child = value[key]
            child_pointer = pointer + "/" + _json_pointer_token(key)
            if _is_sensitive_name(key) and _is_non_empty(child):
                findings.append(Finding(source, child_pointer, "non-empty sensitive field"))
            _audit_json(child, source, child_pointer, findings)
        return
    if isinstance(value, list):
        for index, child in enumerate(value):
            _audit_json(child, source, pointer + "/" + str(index), findings)
        return
    _audit_embedded_url(value, source, pointer or "/", findings)


def _assignment_is_non_empty(raw_value):
    value = raw_value.strip()
    if not value or value in ('""', "''", "~") or value.lower() in ("null", "none"):
        return False
    return True


def _audit_text_assignments(text, source, findings):
    for line_number, line in enumerate(text.splitlines(), 1):
        stripped = line.lstrip()
        if not stripped or stripped.startswith("#") or stripped.startswith(";"):
            continue
        match = ASSIGNMENT_RE.match(line)
        if match and _is_sensitive_name(match.group(1)) and _assignment_is_non_empty(match.group(2)):
            findings.append(Finding(source, "line " + str(line_number), "non-empty sensitive assignment"))


def _audit_file_bytes(source, data, findings, errors):
    for marker in PRIVATE_KEY_MARKERS:
        if marker in data:
            findings.append(Finding(source, "byte content", "private key marker"))
            break

    lower_name = source.lower()
    if lower_name.endswith(".json"):
        try:
            document = json.loads(data.decode("utf-8-sig"))
        except (UnicodeDecodeError, ValueError) as exc:
            errors.append(source + ": invalid JSON: " + str(exc))
            return
        _audit_json(document, source, "", findings)
        return

    extension = os.path.splitext(lower_name)[1]
    if extension in TEXT_CONFIG_EXTENSIONS:
        try:
            text = data.decode("utf-8-sig")
        except UnicodeDecodeError as exc:
            errors.append(source + ": invalid UTF-8 text config: " + str(exc))
            return
        _audit_text_assignments(text, source, findings)


def _read_bounded(stream, source):
    data = stream.read(MAX_AUDIT_FILE_BYTES + 1)
    if len(data) > MAX_AUDIT_FILE_BYTES:
        raise ValueError(source + ": file exceeds audit size limit")
    return data


def _looks_like_tar_name(path):
    return path.lower().endswith(TAR_EXTENSIONS)


def _audit_directory(path, findings, errors):
    scanned = 0
    for root, directories, files in os.walk(path, followlinks=False):
        directories[:] = sorted(name for name in directories if not os.path.islink(os.path.join(root, name)))
        for name in sorted(files):
            file_path = os.path.join(root, name)
            if os.path.islink(file_path):
                continue
            display = os.path.relpath(file_path, path).replace(os.sep, "/")
            source = path.replace(os.sep, "/").rstrip("/") + "/" + display
            try:
                if _looks_like_tar_name(file_path):
                    scanned += _audit_tar(file_path, findings, errors)
                    continue
                with open(file_path, "rb") as stream:
                    data = _read_bounded(stream, source)
                _audit_file_bytes(source, data, findings, errors)
                scanned += 1
            except (IOError, OSError, ValueError) as exc:
                errors.append(str(exc))
    return scanned


def _audit_tar_members(archive, source_prefix, findings, errors, depth):
    scanned = 0
    for member in sorted(archive.getmembers(), key=lambda item: item.name):
        if not member.isfile():
            continue
        source = source_prefix + "!" + member.name
        if member.size > MAX_AUDIT_FILE_BYTES:
            errors.append(source + ": file exceeds audit size limit")
            continue
        stream = archive.extractfile(member)
        if stream is None:
            errors.append(source + ": archive member could not be read")
            continue
        data = _read_bounded(stream, source)
        scanned += 1
        if _looks_like_tar_name(member.name):
            if depth >= MAX_AUDIT_ARCHIVE_DEPTH:
                errors.append(source + ": nested archive depth limit exceeded")
                continue
            try:
                with tarfile.open(fileobj=io.BytesIO(data), mode="r:*") as nested:
                    scanned += _audit_tar_members(nested, source, findings, errors, depth + 1)
            except (IOError, OSError, tarfile.TarError, ValueError) as exc:
                errors.append(source + ": nested archive audit failed: " + str(exc))
        else:
            _audit_file_bytes(source, data, findings, errors)
    return scanned


def _audit_tar(path, findings, errors):
    scanned = 0
    try:
        with tarfile.open(path, "r:*") as archive:
            source = path.replace(os.sep, "/")
            scanned += _audit_tar_members(archive, source, findings, errors, 0)
    except (IOError, OSError, tarfile.TarError, ValueError) as exc:
        errors.append(path + ": archive audit failed: " + str(exc))
    return scanned


def audit_paths(paths):
    findings = []
    errors = []
    scanned = 0
    for path in paths:
        if os.path.isdir(path):
            scanned += _audit_directory(path, findings, errors)
        elif os.path.isfile(path) and (_looks_like_tar_name(path) or tarfile.is_tarfile(path)):
            scanned += _audit_tar(path, findings, errors)
        elif os.path.isfile(path):
            try:
                with open(path, "rb") as stream:
                    data = _read_bounded(stream, path)
                _audit_file_bytes(path, data, findings, errors)
                scanned += 1
            except (IOError, OSError, ValueError) as exc:
                errors.append(str(exc))
        else:
            errors.append(path + ": audit input does not exist")
    findings.sort(key=lambda item: (item.source, item.location, item.reason))
    errors.sort()
    return AuditResult(findings, errors, scanned)


def run(paths, stdout=None, stderr=None):
    stdout = stdout or sys.stdout
    stderr = stderr or sys.stderr
    result = audit_paths(paths)
    for finding in result.findings:
        print(
            "credential audit finding: {}:{}: {}".format(
                finding.source,
                finding.location,
                finding.reason,
            ),
            file=stderr,
        )
    for error in result.errors:
        print("credential audit error: " + error, file=stderr)
    print(
        "credential audit scannedFiles={} findings={} errors={}".format(
            result.scanned_files,
            len(result.findings),
            len(result.errors),
        ),
        file=stdout,
    )
    if result.errors:
        return 2
    if result.findings:
        return 1
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("paths", nargs="+", help="release directory, file, or tar archive")
    args = parser.parse_args(argv)
    return run(args.paths)


if __name__ == "__main__":
    sys.exit(main())
