#!/usr/bin/env python3
"""Offline prepare/capture, then one explicit isolated 22.11 build campaign.

No command defaults to SSH. Python 3 standard library suffices offline;
build additionally requires paramiko and a verified SSH known_hosts entry.
"""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import re
import shlex
import struct
import subprocess
import sys
import tarfile
import time
import uuid


ROOT = Path(__file__).resolve().parents[1]
DIRS = ("src", "include", "tools", "toolchains", "cmake", "test-lab")
SUFFIXES = {".cpp", ".cc", ".c", ".h", ".hpp", ".inc", ".ipp", ".tpp", ".tcc", ".S", ".s", ".cmake", ".sh", ".py", ".ps1", ".in", ".qrc", ".ui"}
MINIMUM = set("MqttDriver MqttForwarder EventEngine EventStore edge_gateway event_store_runtime event_store_client event_store_producer event_store_sender event_stats_reader event_store_test event_store_delivery_test event_store_client_test event_store_producer_test event_engine_ipc_test event_store_sender_test event_store_transport_test event_history_projection_test event_store_history_runtime_test event_store_stats_test event_stats_reader_test mqtt_event_stats_test sqlite_failure_fixture sqlite_outbox_failure_fixture sqlite_writer_failure_test sqlite_outbox_failure_test mqtt_event_outbox_target_test mqtt_driver_service_test mqtt_forwarder_service_test event_engine_service_test builtin_mqtt_driver_publisher_test config_loader_test".split())
PREFIXES = ("event_store", "event_history", "event_engine_ipc", "event_stats", "mqtt_event", "sqlite_")
CONFIG_EXAMPLES = ("config/examples/mqtt-forward-disabled.json", "config/runtime/apps/mqtt-service.json",
                   "config/factory/runtime/apps/mqtt-service.json")


def credential_key(key):
    key = re.sub(r'[^a-z0-9]', '', key.lower())
    return any(word in key for word in ('password', 'passwd', 'secret', 'token', 'credential', 'privatekey', 'accesskey', 'apikey')) or key in ('username', 'user', 'pwd')


def validate_config_example(data):
    """Fail closed on nonempty credential fields/patterns without displaying values."""
    obj = json.loads(data)
    def visit(value, key=''):
        if isinstance(value, dict):
            for k, v in value.items():
                visit(v, re.sub(r'[^a-z0-9]', '', k.lower()))
        elif isinstance(value, list):
            for item in value:
                visit(item, key)
        else:
            sensitive = credential_key(key)
            require(not sensitive or value is None or value == '' or value is False, 'nonempty credential field in explicit config example; value redacted')
            if isinstance(value, str):
                require(not re.search(r'://[^/\s]+@|-----BEGIN [A-Z ]*PRIVATE KEY|\bAKIA[A-Z0-9]{16}\b|\bghp_[A-Za-z0-9]{20,}', value),
                        'embedded credential pattern in explicit config example; value redacted')
    visit(obj)
    return {'credentialCheckPassed': True, 'valuesPrinted': False}


def config_fixture(data):
    """Archive sanitized test data, pin original bytes, and verify the exact field delta."""
    original = json.loads(data)
    changed = []
    def redact(value, key='', path=''):
        if isinstance(value, dict):
            return {k: redact(v, k, path + '/' + k) for k, v in value.items()}
        if isinstance(value, list):
            return [redact(v, key, path + '/' + str(i)) for i, v in enumerate(value)]
        if credential_key(key) and value is not None and value != '' and value is not False:
            changed.append(path)
            return ''
        return value
    redacted = redact(original)
    def verify_delta(a, b, key=''):
        if isinstance(a, dict):
            require(isinstance(b, dict) and a.keys() == b.keys(), 'config fixture changed object structure')
            for k in a:
                verify_delta(a[k], b[k], k)
        elif isinstance(a, list):
            require(isinstance(b, list) and len(a) == len(b), 'config fixture changed list structure')
            for x, y in zip(a, b):
                verify_delta(x, y, key)
        elif a != b:
            require(credential_key(key) and b == '', 'config fixture changed noncredential field')
    encoded = (json.dumps(redacted, ensure_ascii=False, indent=2) + '\n').encode('utf-8')
    verify_delta(original, json.loads(encoded))
    validate_config_example(encoded)
    return encoded, {'inputKind': 'redacted-test-fixture', 'sourceSha256': sha(data),
                     'redactedCredentialFields': changed, 'onlyCredentialFieldsChanged': True}


def source_content(root, name, meta):
    data = (Path(root) / name).read_bytes()
    if meta.get('inputKind') == 'redacted-test-fixture':
        require(name in CONFIG_EXAMPLES and sha(data) == meta['sourceSha256'], 'original config changed since plan')
        data, description = config_fixture(data)
        require(all(meta[k] == v for k, v in description.items()), 'config fixture transformation drift')
    return data


def sha(data):
    return hashlib.sha256(data).hexdigest()


def digest(path):
    h = hashlib.sha256()
    with open(path, "rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def save(path, value):
    with Path(path).open("x", encoding="utf-8") as stream:
        json.dump(value, stream, ensure_ascii=False, indent=2)
        stream.write("\n")


def read(path):
    return json.loads(Path(path).read_text(encoding="utf-8"))


def require(ok, message):
    if not ok:
        raise ValueError(message)


def fresh(path):
    path = Path(path).resolve()
    path.mkdir(parents=True, exist_ok=False)
    return path


def safe_name(name):
    p = PurePosixPath(name)
    return bool(name) and not p.is_absolute() and ".." not in p.parts and "\\" not in name and ":" not in name and str(p) == name


def git(root, *args):
    return subprocess.check_output(["git", "--no-optional-locks", "-c", "core.fsmonitor=false", *args], cwd=root).decode("utf-8").strip()


def inventory(root):
    result = {}
    for base in [root] + [root / d for d in DIRS if (root / d).exists()]:
        paths = base.iterdir() if base == root else base.rglob("*")
        for p in sorted(paths):
            rel = p.relative_to(root).as_posix()
            if any(part in {"__pycache__", ".git", "node_modules", ".venv"} for part in p.relative_to(root).parts):
                continue
            require(not p.is_symlink(), "symlink in source scope: " + rel)
            if not p.is_file() or (p.suffix not in SUFFIXES and p.name != "CMakeLists.txt"):
                continue
            require(safe_name(rel), "unsafe source path: " + rel)
            result[rel] = {"sha256": digest(p), "size": p.stat().st_size,
                           "mode": 0o755 if p.suffix == ".sh" else 0o644}
    # Only these three repository examples are allowed; never recurse into config/.
    for rel in CONFIG_EXAMPLES:
        p = root / rel
        require(p.is_file() and not p.is_symlink() and p.resolve() == p, 'missing/nonregular explicit config example: ' + rel)
        data, description = config_fixture(p.read_bytes())
        result[rel] = dict(description, sha256=sha(data), size=len(data), mode=0o644)
    return dict(sorted(result.items()))


def cmake_contract(root, files):
    targets = {}
    blockers = []
    for name in files:
        if Path(name).name != "CMakeLists.txt" and not name.endswith(".cmake"):
            continue
        source = (root / name).read_text(encoding="utf-8-sig")
        source = re.sub(r"#\[\[.*?\]\]", "", source, flags=re.S)
        source = re.sub(r"#[^\n]*", "", source)
        for match in re.finditer(r"\badd_(executable|library)\s*\(([^()]*)\)", source, re.I):
            tokens = shlex.split(match[2], posix=True)
            if not tokens:
                continue
            target = tokens[0]
            if target not in MINIMUM and not target.startswith(PREFIXES):
                continue
            if not re.fullmatch(r"[A-Za-z0-9_]+", target):
                blockers.append("dynamic target: " + target)
                continue
            kind = "exe" if match[1].lower() == "executable" else ("shared" if "SHARED" in tokens else "static")
            if target in targets:
                blockers.append("duplicate selected target: " + target)
            targets[target] = {"kind": kind, "artifact": target if kind == "exe" else "lib" + target + (".so" if kind == "shared" else ".a")}
            for token in tokens[1:]:
                if "$" not in token and Path(token).suffix in {".cpp", ".c", ".cc", ".h", ".hpp"}:
                    source_path = (PurePosixPath(name).parent / token).as_posix()
                    if source_path not in files:
                        blockers.append("missing literal source: " + source_path)
    blockers += ["missing required target: " + t for t in sorted(MINIMUM - targets.keys())]
    for name in files:
        p = Path(name)
        if name.startswith("tools/") and p.suffix == ".cpp" and p.stem.startswith(PREFIXES) and p.stem.endswith("_test") and p.stem not in targets:
            blockers.append("unregistered test source: " + name)
    return dict(sorted(targets.items())), blockers


def plan(root):
    root = Path(root).resolve()
    files = inventory(root)
    targets, blockers = cmake_contract(root, files)
    for required in ("CMakeLists.txt", "tools/build_edge_aarch64.sh", "toolchains/aarch64-linux-gnu.cmake", "tools/event_store_cross_build.py"):
        if required not in files:
            blockers.append("missing build input: " + required)
    tracked = set(git(root, "ls-files", "-z").split("\0"))
    result = {"schema": 1, "mode": "offline-plan-not-frozen", "files": files,
              "targets": targets, "blockers": blockers, "ready": not blockers,
              "untrackedIncluded": sorted(set(files) - tracked),
              "git": {"commit": git(root, "rev-parse", "HEAD"),
                      "branch": git(root, "rev-parse", "--abbrev-ref", "HEAD"),
                      "status": git(root, "status", "--porcelain", "--untracked-files=all")},
              "sourcePolicy": {"directories": DIRS, "suffixes": sorted(SUFFIXES),
                               "explicitConfigExamples": CONFIG_EXAMPLES, "configCredentialCheck": "redacted test fixtures only; original SHA pinned, only credential fields emptied; no original config bytes archived",
                               "rootFiles": "CMakeLists.txt and allowed suffixes", "symlinks": "reject"}}
    after = inventory(root)
    changed = sorted(n for n in set(files) | set(after) if files.get(n) != after.get(n))
    if changed:
        result["blockers"].append("sources changed during prepare: " + ", ".join(changed))
        result["ready"] = False
    return result


def capture(root, plan_path, expected, out, frozen):
    require(frozen, "capture requires --source-frozen")
    require(digest(plan_path) == expected, "plan SHA mismatch")
    contract = read(plan_path)
    require(contract["ready"] and not contract["blockers"], "plan has unresolved blockers")
    current = plan(root)
    require(current["ready"], "current source inventory is not ready: " + "; ".join(current["blockers"]))
    require(current["files"] == contract["files"] and current["targets"] == contract["targets"], "source/target inventory drift since plan")
    out = fresh(out)
    archive = out / "source.tar.gz"
    with tarfile.open(archive, "x:gz") as tar:
        for name, meta in contract["files"].items():
            data = source_content(root, name, meta)
            require(sha(data) == meta["sha256"], "source changed during capture: " + name)
            info = tarfile.TarInfo(name)
            info.size, info.mode, info.mtime = len(data), meta["mode"], 0
            tar.addfile(info, io.BytesIO(data))
    require(inventory(Path(root)) == contract["files"], "source inventory changed during capture")
    manifest = dict(current, mode="frozen-source", archiveSha256=digest(archive), planSha256=expected)
    save(out / "manifest.json", manifest)
    verify(out, digest(out / "manifest.json"))
    return {"snapshot": str(out), "manifestSha256": digest(out / "manifest.json"), "files": len(manifest["files"]), "targets": len(manifest["targets"])}


def verify(snapshot, expected):
    snapshot = Path(snapshot)
    require(digest(snapshot / "manifest.json") == expected, "manifest SHA mismatch")
    m = read(snapshot / "manifest.json")
    require(m["schema"] == 1 and m["mode"] == "frozen-source" and m["ready"] and not m["blockers"], "invalid frozen contract")
    require(digest(snapshot / "source.tar.gz") == m["archiveSha256"], "archive SHA mismatch")
    require(MINIMUM <= m["targets"].keys(), "missing required build targets")
    for target, meta in m["targets"].items():
        require(re.fullmatch(r"[A-Za-z0-9_]+", target) and safe_name(meta["artifact"]), "unsafe target")
    seen = set()
    with tarfile.open(snapshot / "source.tar.gz") as tar:
        for member in tar:
            require(member.isfile() and safe_name(member.name) and member.name not in seen, "unsafe/duplicate tar member")
            seen.add(member.name)
            meta = m["files"].get(member.name)
            require(meta is not None, "extra archive member")
            require(member.size == meta["size"] and member.mode == meta["mode"], "archive metadata mismatch")
            require(sha(tar.extractfile(member).read()) == meta["sha256"], "archive member SHA mismatch")
    require(seen == m["files"].keys(), "missing archive members")
    return m


def elf(data):
    require(len(data) >= 20 and data[:6] == b"\x7fELF\x02\x01" and struct.unpack_from("<H", data, 18)[0] == 183, "not ELF64 little-endian AArch64")


def check_artifact(path, kind):
    data = Path(path).read_bytes()
    if kind != "static":
        elf(data)
        return
    require(data[:8] == b"!<arch>\n", "not a regular ar archive")
    pos, objects = 8, 0
    while pos < len(data):
        header = data[pos:pos + 60]
        require(len(header) == 60 and header[58:] == b"`\n", "invalid ar header")
        size = int(header[48:58])
        name = header[:16].decode("ascii").strip()
        body = data[pos + 60:pos + 60 + size]
        require(len(body) == size, "truncated ar member")
        if name not in ("/", "//", "/SYM64/"):
            elf(body)
            objects += 1
        pos += 60 + size + size % 2
    require(pos == len(data) and objects > 0, "empty/invalid static archive")


# Executed only by the explicit build subcommand on the private remote tree.
REMOTE_VERIFY = '''import hashlib,json,pathlib,sys
r=pathlib.Path(sys.argv[1]); m=json.loads((r/'manifest.json').read_text())
for n,v in m['files'].items():
 p=r/'source'/n
 assert p.is_file() and not p.is_symlink() and hashlib.sha256(p.read_bytes()).hexdigest()==v['sha256'], n
actual={p.relative_to(r/'source').as_posix() for p in (r/'source').rglob('*') if p.is_file()}
assert actual==set(m['files']), 'source membership changed'
print('source hashes and membership verified')
'''

REMOTE_CLEANUP = '''import json,os,pathlib,signal,sys,time
root=sys.argv[1]; marker=('GATEWAY_EVENT_CROSS_ROOT='+root).encode()
def identity(pid):
 try:
  p=pathlib.Path('/proc')/str(pid)
  exe=os.readlink(str(p/'exe')); cwd=os.readlink(str(p/'cwd'))
  marked=marker in (p/'environ').read_bytes().split(bytes([0]))
  ticks=(p/'stat').read_text().rsplit(')',1)[1].split()[19]
  if exe.startswith(root+'/') or (marked and (cwd==root or cwd.startswith(root+'/'))):
   return (pid,ticks,exe)
 except (OSError,ValueError): pass
def scan():
 return [v for p in pathlib.Path('/proc').iterdir() if p.name.isdigit() for v in [identity(int(p.name))] if v]
killed=[]
for sig in (signal.SIGTERM,signal.SIGKILL):
 for item in scan():
  if identity(item[0])==item:
   try: os.kill(item[0],sig); killed.append(item)
   except ProcessLookupError: pass
 deadline=time.monotonic()+3
 while scan() and time.monotonic()<deadline: time.sleep(.1)
remaining=scan()
print(json.dumps({'terminated':killed,'remaining':remaining}))
sys.exit(1 if remaining else 0)
'''


def attempt(snapshot):
    with (Path(snapshot) / "build-attempt.json").open("x", encoding="utf-8") as f:
        json.dump({"startedUnix": time.time(), "oneCampaign": True}, f)


def build(args):
    require(args.execute, "build requires --execute")
    snapshot = args.snapshot.resolve()
    m = verify(snapshot, args.manifest_sha256)
    require(m["files"]["tools/event_store_cross_build.py"]["sha256"] == digest(__file__), "runner differs from frozen tool")
    import paramiko
    out = fresh(args.output)
    attempt(snapshot)
    remote = "/srv/build/event-store-cross-" + time.strftime("%Y%m%d-%H%M%S", time.gmtime()) + "-" + uuid.uuid4().hex
    q = shlex.quote
    client = paramiko.SSHClient()
    client.load_system_host_keys()
    client.set_missing_host_key_policy(paramiko.RejectPolicy())
    result = {"success": False, "remote": remote, "manifestSha256": args.manifest_sha256, "operations": [], "artifacts": {}, "deployed": False}

    def run(command, name, limit=120):
        entry = {"name": name, "exitCode": None}
        result["operations"].append(entry)
        channel = client.get_transport().open_session(timeout=15)
        channel.set_combine_stderr(True)
        start = time.monotonic()
        output = bytearray()
        try:
            channel.exec_command(command)
            with (out / (name + ".log")).open("xb") as log:
                while True:
                    while channel.recv_ready():
                        chunk = channel.recv(65536)
                        log.write(chunk)
                        log.flush()
                        output.extend(chunk)
                    if channel.exit_status_ready() and not channel.recv_ready():
                        break
                    require(time.monotonic() - start < limit, "SSH operation timeout: " + name + "; remote timeout remains active")
                    time.sleep(0.1)
            entry["exitCode"] = channel.recv_exit_status()
            require(entry["exitCode"] == 0, "remote command failed: " + name)
            print(name + ": OK", flush=True)
            return output.decode("utf-8", "replace")
        finally:
            entry["seconds"] = time.monotonic() - start
            channel.close()

    def remote_sha(path, name):
        return run("sha256sum -- " + q(path), name).split()[0]

    connected = False
    created = False
    shared_before = None
    shared_git = "git --no-optional-locks -c safe.directory=/srv/build/Gateway-zk -C /srv/build/Gateway-zk "
    shared_cmd = shared_git + "rev-parse HEAD; " + shared_git + "symbolic-ref --short HEAD; " + shared_git + "status --porcelain --untracked-files=all"
    try:
        client.connect("192.168.22.11", username=args.user, password=os.environ.get("GATEWAY_BUILD_PASSWORD"), timeout=15, auth_timeout=15, banner_timeout=15)
        connected = True
        shared_before = run("set -e; " + shared_cmd, "shared-before")
        run("mkdir -- " + q(remote) + " && mkdir -- " + q(remote + "/source"), "mkdir")
        created = True
        with client.open_sftp() as sftp:
            for name in ("manifest.json", "source.tar.gz"):
                sftp.put(str(snapshot / name), remote + "/" + name)
                require(remote_sha(remote + "/" + name, "upload-" + name.replace(".", "-")) == digest(snapshot / name), "upload SHA mismatch")
        run("tar --no-same-owner -xzf " + q(remote + "/source.tar.gz") + " -C " + q(remote + "/source"), "extract")
        verify_cmd = "python3 -c " + q(REMOTE_VERIFY) + " " + q(remote)
        run(verify_cmd, "source-before")
        compiler = "/home/tronlong/Linux/SZR/aarch64/gcc-linaro-6.3.1-2017.05-x86_64_aarch64-linux-gnu/bin/aarch64-linux-gnu-g++"
        run("uname -a && " + compiler + " --version && sha256sum " + compiler + " && cmake --version", "toolchain")
        run("chown -R tronlong:tronlong -- " + q(remote), "private-owner")
        for target in m["targets"]:
            command = ("cd " + q(remote + "/source") + " && timeout --signal=TERM --kill-after=30s " + str(args.target_timeout) + "s sudo -u tronlong env -i GATEWAY_EVENT_CROSS_ROOT=" + q(remote) + " PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin bash tools/build_edge_aarch64.sh --jobs " + str(args.jobs) + " --build-dir " + q(remote + "/build") + " " + q(target))
            run(command, "build-" + target, args.target_timeout + 90)
        run(verify_cmd, "source-after")
        with client.open_sftp() as sftp:
            binaries = fresh(out / "binaries")
            for target, meta in m["targets"].items():
                name = meta["artifact"]
                path = remote + "/build/" + name
                expected = remote_sha(path, "artifact-" + target)
                sftp.get(path, str(binaries / name))
                require(digest(binaries / name) == expected, "download SHA mismatch: " + name)
                check_artifact(binaries / name, meta["kind"])
                result["artifacts"][name] = {"sha256": expected, "architecture": "AArch64", "kind": meta["kind"]}
        result["success"] = True
    except Exception as exc:
        result["error"] = str(exc)
        raise
    finally:
        if created:
            try:
                cleanup = run("python3 -c " + q(REMOTE_CLEANUP) + " " + q(remote), "private-process-cleanup")
                result["processCleanup"] = json.loads(cleanup)
            except Exception as exc:
                result["success"] = False
                result["cleanupError"] = str(exc)
        if connected and shared_before is not None:
            try:
                result["sharedStateUnchanged"] = run("set -e; " + shared_cmd, "shared-after") == shared_before
                if not result["sharedStateUnchanged"]:
                    result["success"] = False
                    result["sharedStateNote"] = "shared state changed; possible concurrent owner activity, no shared writes by this tool"
            except Exception as exc:
                result["success"] = False
                result["sharedCheckError"] = str(exc)
        client.close()
        save(out / "build-result.json", result)
    require(result["success"], "build evidence incomplete; see build-result.json")
    return result


def selftest(out):
    out = fresh(out)
    root = fresh(out / "fixture")
    for d in DIRS:
        (root / d).mkdir()
    # Synthetic sources only: the real worktree is never frozen by selftest.
    def put(name, data):
        p = root / name
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(data, encoding="utf-8")
    put("CMakeLists.txt", "\n".join("add_executable(" + t + " tools/fixture.cpp)" for t in sorted(MINIMUM)))
    put("tools/fixture.cpp", "int main() {return 0;}\n")
    for name in CONFIG_EXAMPLES:
        put(name, '{}\n')
    put("tools/build_edge_aarch64.sh", "#!/bin/bash\nexit 0\n")
    put("tools/event_store_cross_build.py", Path(__file__).read_text(encoding="utf-8"))
    put("toolchains/aarch64-linux-gnu.cmake", "set(CMAKE_SYSTEM_PROCESSOR aarch64)\n")
    subprocess.run(["git", "init", "-q", str(root)], check=True)
    subprocess.run(["git", "-C", str(root), "-c", "user.name=fixture", "-c", "user.email=fixture@invalid", "commit", "--allow-empty", "-qm", "fixture"], check=True)
    tests = []
    def rejects(name, action):
        try:
            action()
        except (ValueError, FileExistsError):
            tests.append({"name": name, "passed": True})
        else:
            raise AssertionError("expected rejection: " + name)
    p = plan(root)
    save(out / "plan.json", p)
    pin = digest(out / "plan.json")
    require("tools/fixture.cpp" in p["untrackedIncluded"], "untracked file omitted")
    tests.append({"name": "untracked source included", "passed": True})
    rejects("freeze gate", lambda: capture(root, out / "plan.json", pin, out / "no-freeze", False))
    rejects("plan hash gate", lambda: capture(root, out / "plan.json", "0" * 64, out / "bad-pin", True))
    c = capture(root, out / "plan.json", pin, out / "snapshot", True)
    verify(out / "snapshot", c["manifestSha256"])
    tests.append({"name": "synthetic capture exact member hash verification", "passed": True})
    remote_fixture = fresh(out / "remote-fixture")
    (remote_fixture / "manifest.json").write_bytes((out / "snapshot/manifest.json").read_bytes())
    with tarfile.open(out / "snapshot/source.tar.gz") as tar:
        for member in tar:
            target = remote_fixture / "source" / member.name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(tar.extractfile(member).read())
    checked = subprocess.run([sys.executable, "-c", REMOTE_VERIFY, str(remote_fixture)], capture_output=True, text=True)
    require(checked.returncode == 0, "remote hash verifier fixture failed: " + checked.stderr)
    (out / "remote-verifier.log").write_text(checked.stdout, encoding="utf-8")
    (remote_fixture / "source/tools/fixture.cpp").write_text("tamper", encoding="utf-8")
    checked = subprocess.run([sys.executable, "-c", REMOTE_VERIFY, str(remote_fixture)], capture_output=True, text=True)
    require(checked.returncode != 0, "remote verifier ignored tamper")
    compile(REMOTE_CLEANUP, "remote-cleanup", "exec")
    tests.append({"name": "remote verifier fixture and tamper rejection", "passed": True})
    rejects("preserve existing output", lambda: capture(root, out / "plan.json", pin, out / "snapshot", True))
    put("tools/fixture.cpp", "// edited\n")
    rejects("edited source drift", lambda: capture(root, out / "plan.json", pin, out / "edited", True))
    (root / "tools/fixture.cpp").unlink()
    rejects("deleted source drift", lambda: capture(root, out / "plan.json", pin, out / "deleted", True))
    put("tools/fixture.cpp", "int main() {return 0;}\n")
    put("src/new_untracked.cpp", "// new\n")
    rejects("new source drift", lambda: capture(root, out / "plan.json", pin, out / "drift", True))
    put("tools/event_store_new_test.cpp", "int main() {}\n")
    require(any("unregistered" in b for b in plan(root)["blockers"]), "unregistered new test accepted")
    tests.append({"name": "unregistered test blocker", "passed": True})
    before = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    put("CMakeLists.txt", before.replace("add_executable(EventStore tools/fixture.cpp)", ""))
    require("missing required target: EventStore" in plan(root)["blockers"], "required target removal accepted")
    tests.append({"name": "required target removal blocker", "passed": True})
    put("CMakeLists.txt", before)
    (root / "src/link.cpp").symlink_to(root / "tools/fixture.cpp")
    rejects("source symlink rejected", lambda: inventory(root))
    (root / "src/link.cpp").unlink()
    attempt(out / "snapshot")
    rejects("one campaign only", lambda: attempt(out / "snapshot"))
    rejects("manifest hash gate", lambda: verify(out / "snapshot", "0" * 64))
    with (out / "snapshot/source.tar.gz").open("ab") as f:
        f.write(b"tamper")
    rejects("archive tamper", lambda: verify(out / "snapshot", c["manifestSha256"]))
    require(not safe_name("../escape") and not safe_name("/absolute") and not safe_name("a\\b"), "unsafe path accepted")
    tests.append({"name": "path traversal rejection", "passed": True})
    good = bytearray(64)
    good[:6] = b"\x7fELF\x02\x01"
    struct.pack_into("<H", good, 18, 183)
    elf(good)
    header = ("fixture.o/".ljust(16) + "0".ljust(12) + "0".ljust(6) + "0".ljust(6) + "100644".ljust(8) + str(len(good)).ljust(10) + "`\n").encode("ascii")
    (out / "libfixture.a").write_bytes(b"!<arch>\n" + header + good)
    check_artifact(out / "libfixture.a", "static")
    tests.append({"name": "AArch64 static archive members", "passed": True})
    struct.pack_into("<H", good, 18, 62)
    rejects("x86 artifact rejected", lambda: elf(good))
    (out / "libwrong.a").write_bytes(b"!<arch>\n" + header + good)
    rejects("x86 archive rejected", lambda: check_artifact(out / "libwrong.a", "static"))
    for cmd in (["--help"], ["build", "--help"], ["capture", "--help"]):
        completed = subprocess.run([sys.executable, str(Path(__file__).resolve()), *cmd], capture_output=True, text=True)
        require(completed.returncode == 0, "CLI help failed")
        (out / ((cmd[0].strip("-") or "help") + "-help.log")).write_text(completed.stdout + completed.stderr, encoding="utf-8")
    tests.append({"name": "CLI help offline", "passed": True})
    for command, extra in (("capture", ["--plan", str(out / "plan.json"), "--plan-sha256", pin]), ("build", ["--snapshot", str(out / "snapshot"), "--manifest-sha256", c["manifestSha256"]])):
        completed = subprocess.run([sys.executable, str(Path(__file__).resolve()), command, *extra, "--output", str(out / (command + "-forbidden"))], capture_output=True, text=True)
        require(completed.returncode == 1 and "requires --" in completed.stderr, "CLI execution gate failed")
        (out / (command + "-gate.log")).write_text(completed.stdout + completed.stderr, encoding="utf-8")
        tests.append({"name": command + " CLI execution gate", "passed": True})
    result = {"passed": True, "tests": tests, "remoteConnections": 0, "realSourceFrozen": False, "fixtureSnapshotIntentionallyTampered": True}
    save(out / "selftest.json", result)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("prepare", help="read-only source and target inventory; no freeze or SSH")
    p.add_argument("--root", type=Path, default=ROOT)
    p.add_argument("--output", type=Path, required=True)
    p = sub.add_parser("capture", help="explicitly frozen sources only; local archive")
    p.add_argument("--root", type=Path, default=ROOT)
    p.add_argument("--plan", type=Path, required=True)
    p.add_argument("--plan-sha256", required=True)
    p.add_argument("--source-frozen", action="store_true")
    p.add_argument("--output", type=Path, required=True)
    for cmd in ("verify", "build"):
        p = sub.add_parser(cmd)
        p.add_argument("--snapshot", type=Path, required=True)
        p.add_argument("--manifest-sha256", required=True)
        if cmd == "build":
            p.add_argument("--execute", action="store_true")
            p.add_argument("--user", default="root")
            p.add_argument("--jobs", type=int, default=4)
            p.add_argument("--target-timeout", type=int, default=1200)
            p.add_argument("--output", type=Path, required=True)
    p = sub.add_parser("selftest", help="synthetic fixture capture and negative tests; no SSH")
    p.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.command == "prepare":
        result = plan(args.root)
        out = fresh(args.output)
        save(out / "plan.json", result)
        result = {"ready": result["ready"], "blockers": result["blockers"], "files": len(result["files"]), "targets": list(result["targets"]), "plan": str(out / "plan.json"), "planSha256": digest(out / "plan.json"), "remoteConnections": 0, "sourceFrozen": False}
    elif args.command == "capture":
        result = capture(args.root, args.plan, args.plan_sha256, args.output, args.source_frozen)
    elif args.command == "verify":
        m = verify(args.snapshot, args.manifest_sha256)
        result = {"verified": True, "files": len(m["files"]), "targets": len(m["targets"])}
    elif args.command == "selftest":
        result = selftest(args.output)
    else:
        require(1 <= args.jobs <= 32 and 1 <= args.target_timeout <= 7200, "invalid jobs/timeout")
        result = build(args)
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 2 if result.get("ready") is False else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as error:
        print(type(error).__name__ + ": " + str(error), file=sys.stderr)
        sys.exit(1)
