#!/usr/bin/env python3
"""Local, root-only COMM202600104 r05 canary installer. No SSH/network client.

prepare --spec SPEC --work NEW_DIRECTORY
activate|rollback|verify --work DIRECTORY [--gate GATE]

SPEC (absolute Linux paths; private full configs never appear in stdout):
  release: flat directory with EventStore,EventEngine,MqttDriver,MqttForwarder
           and libsqlite3.so (3.53.4)
  data: NEW canary data directory, already containing migrated events.db/history.db
  identity: device_identity.json with machineCode COMM202600104
  baseline: immutable migrated cutover baseline (independent file, not raw legacy)
  source: extracted legacy WORKSET DB used as migrate --source (not 9GB archive)
  archive_source: original frozen 9GB outbox DB, still at original config path
  extract_manifest: event_store_extract_workset.py event-store-workset-v1 receipt
  history_source: untouched legacy history DB
  migration_report, history_report: JSON stdout from event_store_migrate.py
  history_id: SAME stable namespace used for initial history migration
  lab_template, overlay_template: r05 integrated lab JSON and client overlay JSON
  services: three objects {unit, binary, config, original}; original is the FULL
            local byte-for-byte pre-cutover config copy. unit must match binary
            and the config basename, e.g. event-engine@mqtt-service.service.
  ota_markers: nonempty list including /run/gateway-health-watchdog/applying
              plus site-specific OTA in-progress markers, reviewed by operator
  reserve_bytes: optional free-space floor (minimum 1 GiB)
  start_gate: optional; ONLY /run/event-store-cutover-104.ready is allowed.
              Must be absent at prepare/activate. The wrapper installs runtime
              ConditionPathExists guards for all three clients before unmasking.
              activate publishes this file only after Store Hello succeeds.
              activate immediately checks publication; long-term verify does not
              require it. rollback never writes/removes it. The wrapper removes
              runtime guards and this gate after success, and owns recovery guards.

GATE is operator evidence, not a substitute for the local checks:
  {machineCode: COMM202600104, otaIdle: true, restartInhibited: true,
   expires: Unix seconds no more than 300 seconds ahead}
The main operator must prevent new OTA jobs, watchdog/service auto-start and
other DB access throughout cutover. prepare never stops or starts anything.
activate requires all three old services ALREADY stopped and migration complete.
rollback requires all four services ALREADY stopped; it never stops anything.
It merges increments via the migration CLI into new outputs, then restores old
binaries with DB paths redirected to those outputs. It leaves services stopped.
Original archives, baseline, current canary DBs and their WALs are never deleted.
On any partial activation failure: keep restart inhibition, stop only the four
canary services externally, then run rollback. No snapshot-copy recovery exists.
Do not edit prepared artifacts: their hashes and source paths are sealed in plan.
Run verify explicitly after activation and during observation. A dead Store or
contract failure exits nonzero and persists needs-rollback; Restart=no is
intentional. Operator must stop ONLY the four canary units, then invoke rollback.
This tool does not sample /proc/PID/io; unavailable observer samples remain missing.

Compatibility incident / emergency-review exception (2026-09-08): the operator
reported that legacy binaries rejected ASCII-escaped Unicode during rollback.
Emergency recovery rewrote the active app as literal UTF-8 with unchanged JSON
values, updated the allowed rollback config hash, and restarted only the three
original services before retrospective review. Reported reconciliation: 953
states; 12525 trial events, zero missing/changed/lost ACKs. These are operator
reports, not live verification by this local tool. This local fix must not replace
the already sealed on-device script or silently reseal an existing plan.
"""

import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import socket
import stat
import struct
import subprocess
import sys
import tempfile
import time


IDENTITY = "COMM202600104"
STORE_UNIT = "event-store-canary-104.service"
START_GATE = "/run/event-store-cutover-104.ready"
UNIT_ROOT = Path("/etc/systemd/system")
BINARIES = {"EventEngine": "event-engine", "MqttDriver": "mqtt-driver",
            "MqttForwarder": "mqtt-forwarder"}
OUTBOX = ("mqtt", "offlineBuffer", "eventOutbox", "sqlitePath")
ALARM = ("alarmStore", "sqlitePath")


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def unique(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, "duplicate JSON key")
        result[key] = value
    return result


def read_json(path):
    return json.loads(Path(path).read_text(encoding="utf-8"), object_pairs_hook=unique)


def digest(path):
    h = hashlib.sha256()
    with Path(path).open("rb") as handle:
        for block in iter(lambda: handle.read(4 * 1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def safe_path(value):
    p = Path(value)
    require(p.is_absolute() and re.fullmatch(r"/[A-Za-z0-9_./-]+", str(p)), "unsafe path")
    require(".." not in p.parts and p.resolve() == p, "noncanonical/symlink path")
    return p


def durable(path, content, replace=False):
    path = Path(path)
    require(not path.is_symlink(), "refusing symlink output")
    require(replace or not path.exists(), "output already exists: " + str(path))
    fd, name = tempfile.mkstemp(prefix="." + path.name + ".canary-", dir=path.parent)
    temp = Path(name)
    with os.fdopen(fd, "wb") as handle:
        handle.write(content)
        handle.flush()
        os.fsync(handle.fileno())
    if path.exists():
        info = path.stat()
        os.chmod(temp, stat.S_IMODE(info.st_mode))
        os.chown(temp, info.st_uid, info.st_gid)
    os.replace(temp, path)
    if os.name == "posix":
        fd = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(fd)
        finally:
            os.close(fd)


def save(path, value, replace=False):
    # Legacy app-config parsers accept UTF-8 literals but not Unicode escapes.
    durable(path, (json.dumps(value, indent=2, ensure_ascii=False) + "\n").encode("utf-8"), replace)


def check_start_gate(spec, present=False):
    if "start_gate" not in spec:
        return None
    require(spec["start_gate"] == START_GATE, "start_gate must be " + START_GATE)
    path = safe_path(spec["start_gate"])
    if present:
        require(path.is_file(), "start_gate missing or not a regular file")
    else:
        require(not os.path.lexists(path), "start_gate must be absent")
    return path


def publish_start_gate(spec):
    path = check_start_gate(spec)
    if path is None:
        return
    # Publish complete, fsynced bytes without replacing a concurrently created
    # gate. The temporary file is in /run, on the same filesystem as the gate.
    fd, name = tempfile.mkstemp(prefix=".event-store-cutover-104-", dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as handle:
            handle.write((IDENTITY + " r05 Store Hello verified\n").encode("ascii"))
            handle.flush()
            os.fsync(handle.fileno())
        os.link(name, path)
    finally:
        os.unlink(name)
    fd = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def get(obj, keys):
    for key in keys:
        obj = obj[key]
    return obj


def put(obj, keys, value):
    for key in keys[:-1]:
        require(isinstance(obj.setdefault(key, {}), dict), "non-object config parent")
        obj = obj[key]
    obj[keys[-1]] = value


def diff_paths(a, b, prefix=()):
    if isinstance(a, dict) and isinstance(b, dict):
        result = []
        for key in sorted(a.keys() | b.keys()):
            if key not in a or key not in b:
                result.append(prefix + (key,))
            else:
                result.extend(diff_paths(a[key], b[key], prefix + (key,)))
        return result
    return [] if a == b else [prefix]


def patched(original, overlay, events, history):
    result = copy.deepcopy(original)
    if overlay is not None:
        require(set(overlay) == {"eventStore"}, "overlay must only contain eventStore")
        result["eventStore"] = copy.deepcopy(overlay["eventStore"])
    put(result, OUTBOX, str(events))
    put(result, ALARM, str(history))
    for path in diff_paths(original, result):
        require(path in (OUTBOX, ALARM) or (overlay is not None and path[0] == "eventStore"),
                "config diff outside allowlist")
    return result


def run(argv):
    return subprocess.run([str(a) for a in argv], check=True, text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=600).stdout


def systemctl(*args):
    return run(["systemctl", *args])


def stopped(units):
    for unit in units:
        state = systemctl("show", unit, "--property=ActiveState", "--value").strip()
        require(state in ("inactive", "failed"), "service must already be stopped: " + unit)
        require(systemctl("show", unit, "--property=MainPID", "--value").strip() == "0",
                "service still has PID: " + unit)


def no_accessors(paths):
    # Any open DB/sidecar descriptor is rejected, including readers. Fail closed
    # on permission errors; a writer reservation alone cannot prove offline use.
    targets = set()
    for path in paths:
        for suffix in ("", "-wal", "-shm", "-journal"):
            p = Path(str(path) + suffix)
            if p.exists():
                s = p.stat()
                targets.add((s.st_dev, s.st_ino))
    for process in Path("/proc").iterdir():
        if not process.name.isdigit() or int(process.name) == os.getpid():
            continue
        try:
            for fd in (process / "fd").iterdir():
                try:
                    info = fd.stat()
                except FileNotFoundError:
                    continue
                require((info.st_dev, info.st_ino) not in targets,
                        "DB accessor still present: PID " + process.name)
            maps = (process / "maps").read_text()
            for line in maps.splitlines():
                fields = line.split(None, 5)
                if len(fields) >= 5:
                    major, minor = (int(v, 16) for v in fields[3].split(":"))
                    require((os.makedev(major, minor), int(fields[4])) not in targets,
                            "DB mapping still present: PID " + process.name)
        except FileNotFoundError:
            continue


def gate_check(plan, gate):
    require(read_json(plan["spec"]["identity"])["machineCode"] == IDENTITY, "wrong device identity")
    evidence = read_json(gate)
    require(evidence.get("machineCode") == IDENTITY and evidence.get("otaIdle") is True
            and evidence.get("restartInhibited") is True, "operator OTA/restart gate missing")
    require(0 < evidence["expires"] - time.time() <= 300, "stale/future operator gate")
    for marker in plan["spec"]["ota_markers"]:
        require(not Path(marker).exists(), "OTA marker present")
    for process in Path("/proc").iterdir():
        if not process.name.isdigit():
            continue
        try:
            cmd = (process / "cmdline").read_bytes().split(b"\0")
            require(not any(Path(os.fsdecode(arg)).name in
                            ("ota-apply.sh", "ota-rollback.sh") for arg in cmd), "OTA process present")
        except FileNotFoundError:
            continue


def capacity(plan):
    s = plan["spec"]
    data = Path(s["data"])
    sizes = sum(sum(Path(str(p) + suffix).stat().st_size for suffix in ("", "-wal")
                    if Path(str(p) + suffix).exists()) for p in
                (s["history_source"], s["baseline"], data / "events.db", data / "history.db"))
    # Incremental migration uses snapshots, backups, staging and published output.
    needed = 5 * sizes + max(1024 ** 3, s.get("reserve_bytes", 1024 ** 3))
    require(shutil.disk_usage(data).free >= needed, "insufficient rollback working space")
    require(shutil.disk_usage(UNIT_ROOT).free >= 16 * 1024 ** 2, "insufficient config/unit space")


def unit_files(spec, work):
    release = Path(spec["release"])
    files = {str(UNIT_ROOT / STORE_UNIT): (
        "[Unit]\nDescription=COMM202600104 EventStore r05 authorized canary\n"
        "[Service]\nType=exec\nUser=root\nUMask=0077\n"
        f"ExecStart={release}/EventStore --lab-config {work}/lab.json\n"
        "Restart=no\nTimeoutStartSec=60\nTimeoutStopSec=120\n"
        "KillSignal=SIGTERM\nKillMode=control-group\nSendSIGKILL=no\n")}
    for service in spec["services"]:
        files[str(UNIT_ROOT / (service["unit"] + ".d") / "90-event-store-canary-104.conf")] = (
            f"[Unit]\nRequires={STORE_UNIT}\nAfter={STORE_UNIT}\n"
            "[Service]\nExecStart=\n"
            f"ExecStart={release}/{service['binary']} --app-config {service['config']}\n"
            "TimeoutStopSec=120\nSendSIGKILL=no\n")
    return files


def file_set(path):
    # Match migrate.py: SHM is transient; empty WAL/journal has no records.
    return {suffix: digest(p) for suffix in ("", "-wal", "-journal")
            if (p := Path(str(path) + suffix)).exists() and (not suffix or p.stat().st_size > 0)}


def migration_evidence(spec):
    data = Path(spec["data"])
    reports = [read_json(spec[k]) for k in ("migration_report", "history_report")]
    for report, command, target in zip(reports, ("migrate", "history"),
                                       (data / "events.db", data / "history.db")):
        require(report.get("command") == command and report.get("publication") == "published"
                and report.get("dry_run") is False, "migration report is not a published result")
        require(report.get("target", str(target)) == str(target) and report["target_sha256"] == digest(target),
                "migration output changed")
        require(file_set(target) == {"": report["target_sha256"]}, "migration output has unaccounted WAL/journal")
    require(reports[0]["cutover_baseline"] == spec["baseline"] and
            digest(spec["baseline"]) == reports[0]["target_sha256"], "wrong cutover baseline")
    originals = {}
    for key, report in zip(("source", "history_source"), reports):
        entries = [e for e in report["sources"] if e["path"] == spec[key]]
        require(len(entries) == 1 and entries[0]["files_sha256"] == file_set(spec[key]),
                "legacy source changed since migration")
        originals[spec[key]] = entries[0]["files_sha256"]
    return originals


def archive_metadata(path):
    result = {}
    for suffix in ("", "-wal", "-journal"):
        p = Path(str(path) + suffix)
        if not os.path.lexists(p):
            continue
        info = p.lstat()
        require(stat.S_ISREG(info.st_mode) and info.st_nlink == 1 and not info.st_mode & 0o222,
                "archive/sidecar must stay regular, single-link and read-only")
        require(suffix != "-journal" or info.st_size == 0, "archive has rollback journal")
        if suffix and not info.st_size:
            continue
        result[suffix] = dict(device=info.st_dev, inode=info.st_ino, size=info.st_size,
                              mtime_ns=info.st_mtime_ns, ctime_ns=info.st_ctime_ns)
    require("" in result, "archive missing")
    return result


def extraction_evidence(spec):
    report = read_json(spec["extract_manifest"])
    require(report.get("format") == "event-store-workset-v1" and report.get("mode") == "extract"
            and report.get("status") == "OFFLINE_WORKSET_CANDIDATE"
            and report.get("consistent_snapshot") is True and report.get("source_preserved") is True
            and report.get("machine_code") == IDENTITY, "invalid extraction manifest")
    require(report["source"] == spec["archive_source"] and report["target"] == spec["source"],
            "archive/workset extraction relationship mismatch")
    require(report["target_sha256"] == digest(spec["source"]), "extracted workset changed")
    before = archive_metadata(spec["archive_source"])
    require(set(before) == set(report["origin"]), "archive sidecar set changed")
    for suffix, metadata in before.items():
        receipt = report["origin"][suffix]
        require(all(metadata[k] == receipt[k] for k in ("device", "inode", "size", "mtime_ns")),
                "archive metadata differs from extraction receipt")
        require(digest(spec["archive_source"] + suffix) == receipt["sha256"], "archive hash mismatch")
    require(archive_metadata(spec["archive_source"]) == before, "archive changed during final hash")
    return before


def prepare(spec_path, work):
    s = read_json(spec_path)
    check_start_gate(s)
    for key in ("release", "data", "identity", "baseline", "source", "archive_source", "extract_manifest", "history_source",
                "migration_report", "history_report", "lab_template", "overlay_template"):
        safe_path(s[key])
    require(len(s["services"]) == 3 and {x["binary"] for x in s["services"]} == set(BINARIES),
            "exactly three allowed services required")
    require(s["ota_markers"] and "/run/gateway-health-watchdog/applying" in s["ota_markers"],
            "include watchdog applying marker and reviewed site OTA markers")
    for marker in s["ota_markers"]:
        safe_path(marker)
    require(read_json(s["identity"])["machineCode"] == IDENTITY, "wrong identity")
    require(isinstance(s["history_id"], str) and re.fullmatch(r"[A-Za-z0-9_.:-]+", s["history_id"]),
            "history_id must be the stable migration namespace")
    data, release = Path(s["data"]), Path(s["release"])
    dbs = [Path(s[k]) for k in ("source", "history_source", "baseline", "archive_source")] + [data / "events.db", data / "history.db"]
    require(len({(p.stat().st_dev, p.stat().st_ino) for p in dbs}) == 6, "DB inputs alias")
    require(all(data not in p.parents for p in dbs[:4]), "archives/baseline must be outside canary data")
    require(not Path(s["baseline"]).stat().st_mode & 0o222, "baseline must be read-only")
    originals = migration_evidence(s)
    archive = extraction_evidence(s)
    lab, overlay = read_json(s["lab_template"]), read_json(s["overlay_template"])
    require(lab["laboratoryOnly"] is True and overlay["eventStore"]["backend"] == "ipc-lab",
            "r05 lab protocol markers required")
    require(all(lab[k] == overlay["eventStore"][k] for k in ("storeId", "configGeneration", "storageProfile")),
            "lab/overlay identity mismatch")
    lab.update(databasePath=str(data / "events.db"), historyPath=str(data / "history.db"),
               socketPath=str(data / "events.sock"), sqliteLibraryPath=str(release / "libsqlite3.so"))
    require(len(lab["socketPath"].encode()) < 108, "Unix socket path too long")
    overlay["eventStore"].update(socketPath=lab["socketPath"], producerHealthFile=str(data / "producer-health.json"))
    work = safe_path(str(work))
    require(not work.exists() and data not in work.parents and work not in data.parents, "work must be new and separate")
    configs, hashes = {}, {}
    for item in s["services"]:
        config, original = safe_path(item["config"]), safe_path(item["original"])
        require(re.fullmatch(BINARIES[item["binary"]] + r"@[A-Za-z0-9_-]+\.service", item["unit"]), "unexpected unit")
        require(item["unit"].split("@", 1)[1] == config.stem + ".service", "unit/config instance mismatch")
        require(config != original and digest(config) == digest(original), "full original config copy differs")
        old = read_json(original)
        require(old["identityConfigFile"] == s["identity"], "config identity path mismatch")
        require(get(old, OUTBOX) == s["archive_source"] and get(old, ALARM) == s["history_source"], "unaccounted config DB path")
        configs[str(config)] = {"original": str(original), "activated": patched(old, overlay, data / "events.db", data / "history.db")}
        hashes[str(original)] = digest(original)
    for path in [release / b for b in ("EventStore", *BINARIES)] + [release / "libsqlite3.so", Path(s["baseline"]), Path(s["extract_manifest"]), Path(__file__).with_name("event_store_migrate.py")]:
        safe_path(str(path))
        hashes[str(path)] = digest(path)
    import ctypes
    library = ctypes.CDLL(str(release / "libsqlite3.so"))
    library.sqlite3_libversion.restype = ctypes.c_char_p
    require(library.sqlite3_libversion() == b"3.53.4", "private SQLite is not 3.53.4")
    plan = {"spec": s, "hashes": hashes, "original_sources": originals, "archive_metadata": archive,
            "configs": {}, "units": unit_files(s, work)}
    plan["original_units"] = {item["unit"]: systemctl("cat", item["unit"]) for item in s["services"]}
    capacity(plan)
    require(all(not Path(p).exists() for p in plan["units"]), "canary unit/drop-in already exists")
    check_start_gate(s)
    work.mkdir(mode=0o700)
    save(work / "lab.json", lab)
    plan["hashes"][str(work / "lab.json")] = digest(work / "lab.json")
    review = {}
    for i, (config, values) in enumerate(configs.items()):
        target = work / ("config-" + str(i) + ".json")
        save(target, values["activated"])
        plan["configs"][config] = {"original": values["original"], "activated": str(target)}
        plan["hashes"][str(target)] = digest(target)
        review[config] = ["/" + "/".join(p) for p in diff_paths(read_json(values["original"]), values["activated"])]
    save(work / "allowed-diff.json", review)
    for i, content in enumerate(plan["units"].values()):
        durable(work / ("unit-" + str(i) + ".conf"), content.encode())
    save(work / "plan.json", plan)
    save(work / "state.json", {"phase": "prepared", "plan_sha256": digest(work / "plan.json")})
    return {"phase": "prepared", "review": str(work / "allowed-diff.json"), "data": str(data)}


def load(work):
    state = read_json(work / "state.json")
    require(digest(work / "plan.json") == state["plan_sha256"], "plan changed")
    plan = read_json(work / "plan.json")
    require(not Path(plan["spec"]["baseline"]).stat().st_mode & 0o222, "baseline no longer read-only")
    for path, sha in plan["hashes"].items():
        require(digest(path) == sha, "sealed input changed: " + path)
    return plan, state


def phase(work, state, name):
    state["phase"] = name
    save(work / "state.json", state, True)


def check_sources(plan):
    require(archive_metadata(plan["spec"]["archive_source"]) == plan["archive_metadata"],
            "frozen archive changed; full independent revalidation required")
    actual = {}
    for key in ("source", "history_source"):
        actual[plan["spec"][key]] = file_set(plan["spec"][key])
    require(actual == plan["original_sources"], "source archive changed")


def hello(lab):
    body = json.dumps({"version": "1", "storeId": lab["storeId"],
                       "configGeneration": lab["configGeneration"], "op": "Hello", "args": {}}).encode()
    def receive(peer, length):
        result = b""
        while len(result) < length:
            part = peer.recv(length - len(result))
            require(part, "truncated Hello")
            result += part
        return result
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as peer:
        peer.settimeout(3)
        peer.connect(lab["socketPath"])
        peer.sendall(struct.pack("!I", len(body)) + body)
        size = struct.unpack("!I", receive(peer, 4))[0]
        require(0 < size <= 262144, "invalid Hello frame size")
        value = json.loads(receive(peer, size), object_pairs_hook=unique)
    expected = {"ok": True, "laboratoryOnly": True, "version": "1", "storeId": lab["storeId"],
                "configGeneration": lab["configGeneration"], "sqliteVersion": "3.53.4",
                "synchronous": "2", "storageProfile": lab["storageProfile"],
                "localJournalVersion": "1", "historyProjectionVersion": "1", "historyEnabled": True}
    require(all(type(value.get(k)) is type(v) and value[k] == v for k, v in expected.items()), "Hello contract mismatch")
    return value


def effective_commands(plan, work, running=False):
    release = Path(plan["spec"]["release"])
    commands = [(STORE_UNIT, [str(release / "EventStore"), "--lab-config", str(work / "lab.json")])]
    commands += [(item["unit"], [str(release / item["binary"]), "--app-config", item["config"]])
                 for item in plan["spec"]["services"]]
    for unit, expected in commands:
        value = systemctl("show", unit, "--property=ExecStart", "--value")
        paths = re.findall(r"(?:\{\s*)?path=([^ ;]+)\s*;", value)
        argv = re.findall(r"argv\[\]=([^;]+);", value)
        require(paths == [expected[0]] and len(argv) == 1 and argv[0].split() == expected,
                "effective ExecStart overridden: " + unit)
        if running:
            pid = systemctl("show", unit, "--property=MainPID", "--value").strip()
            require(pid.isdigit() and int(pid) > 0, "missing active PID: " + unit)
            require(Path("/proc", pid, "exe").resolve() == Path(expected[0]), "wrong running binary: " + unit)
            actual = Path("/proc", pid, "cmdline").read_bytes().rstrip(b"\0").split(b"\0")
            require(actual == [os.fsencode(a) for a in expected], "wrong running arguments: " + unit)


def verify(plan, work):
    check_sources(plan)
    for path, values in plan["configs"].items():
        require(digest(path) == digest(values["activated"]), "active config drift")
    for path, content in plan["units"].items():
        require(Path(path).read_text() == content, "active unit drift")
    for unit in [STORE_UNIT] + [s["unit"] for s in plan["spec"]["services"]]:
        require(systemctl("show", unit, "--property=ActiveState", "--value").strip() == "active", "unit not active: " + unit)
    effective_commands(plan, work, running=True)
    value = hello(read_json(work / "lab.json"))
    return {"phase": "active", "hello": value}


def activate(plan, state, work, gate):
    require(state["phase"] == "prepared", "not prepared; partial activation needs offline rollback")
    check_start_gate(plan["spec"])
    gate_check(plan, gate)
    units = [s["unit"] for s in plan["spec"]["services"]]
    stopped(units + [STORE_UNIT])
    s = plan["spec"]
    no_accessors([s["source"], s["archive_source"], s["history_source"], s["baseline"], Path(s["data"]) / "events.db", Path(s["data"]) / "history.db"])
    migration_evidence(s)
    check_sources(plan)
    capacity(plan)
    for path, values in plan["configs"].items():
        require(digest(path) == digest(values["original"]), "config changed after old services stopped")
    require(all(not Path(p).exists() for p in plan["units"]), "canary unit collision")
    for unit, original in plan["original_units"].items():
        require(systemctl("cat", unit) == original, "original unit changed after prepare")
    gate_check(plan, gate)
    check_start_gate(s)
    phase(work, state, "activating")
    for path, values in plan["configs"].items():
        durable(path, Path(values["activated"]).read_bytes(), True)
    for path, content in plan["units"].items():
        Path(path).parent.mkdir(parents=True, exist_ok=True)
        durable(path, content.encode())
    systemctl("daemon-reload")
    effective_commands(plan, work)
    # Dependencies are already checked/started explicitly. Do not enqueue starts
    # or stops for unrelated units through inherited dependency/conflict edges.
    systemctl("--job-mode=ignore-dependencies", "start", STORE_UNIT)
    deadline = time.monotonic() + 60
    while True:
        try:
            hello(read_json(work / "lab.json"))
            break
        except (OSError, RuntimeError, ValueError):
            require(time.monotonic() < deadline, "readiness failed; stop four units externally and rollback")
            time.sleep(0.25)
    publish_start_gate(s)
    check_start_gate(s, present=True)
    for unit in units:
        systemctl("--job-mode=ignore-dependencies", "start", unit)
    result = verify(plan, work)
    phase(work, state, "active")
    return result


def rollback_commands(plan, output):
    s = plan["spec"]
    base = [sys.executable, str(Path(__file__).with_name("event_store_migrate.py"))]
    data = Path(s["data"])
    common = ["--offline", "--backup-dir", str(output / "backups")]
    return [base + ["rollback", "--source", str(data / "events.db"), "--baseline", s["baseline"],
                    "--target", str(output / "events.db")] + common,
            base + ["history", "--source", s["history_source"], "--event-store", str(data / "events.db"),
                    "--projection", str(data / "history.db"), "--history-id", s["history_id"],
                    "--target", str(output / "history.db")] + common]


def rollback(plan, state, work, gate):
    require(state["phase"] in ("activating", "active", "rolling-back", "needs-rollback"), "no cutover to roll back")
    gate_check(plan, gate)
    stopped([STORE_UNIT] + [s["unit"] for s in plan["spec"]["services"]])
    s = plan["spec"]
    data = Path(s["data"])
    no_accessors([s["source"], s["archive_source"], s["history_source"], s["baseline"], data / "events.db", data / "history.db"])
    check_sources(plan)
    capacity(plan)
    for path, content in plan["units"].items():
        require(not Path(path).exists() or Path(path).read_text() == content, "foreign unit edits; manual review required")
    for path, values in plan["configs"].items():
        allowed = [digest(values[k]) for k in ("original", "activated")]
        if path in state.get("rollback_configs", {}):
            allowed.extend(state["rollback_configs"][path])
        require(digest(path) in allowed, "config drift; refusing to discard user changes")
    phase(work, state, "rolling-back")
    # Every retry gets independent outputs. Never infer success from an orphaned
    # target: an interrupted migration may not have returned its final report.
    output = Path(tempfile.mkdtemp(prefix="rollback-", dir=data))
    (output / "backups").mkdir(mode=0o700)
    for command, filename in zip(rollback_commands(plan, output), ("events.db", "history.db")):
        report = json.loads(run(command), object_pairs_hook=unique)
        require(report.get("publication") == "published" and report["target_sha256"] == digest(output / filename),
                "incremental recovery not published")
        save(output / (filename + ".report.json"), report)
    check_sources(plan)
    gate_check(plan, gate)
    staged = {}
    for i, (path, values) in enumerate(plan["configs"].items()):
        config = patched(read_json(values["original"]), None, output / "events.db", output / "history.db")
        target = output / ("config-" + str(i) + ".json")
        save(target, config)
        staged[path] = target
    previous = state.get("rollback_configs", {})
    state["rollback_configs"] = {p: list(set(previous.get(p, []) + [digest(t)])) for p, t in staged.items()}
    state["rollback_output"] = str(output)
    save(work / "state.json", state, True)
    for path, target in staged.items():
        durable(path, target.read_bytes(), True)
    for path in plan["units"]:
        if Path(path).exists():
            Path(path).unlink()  # Only our exact, content-checked systemd artifacts.
            fd = os.open(Path(path).parent, os.O_RDONLY | os.O_DIRECTORY)
            try:
                os.fsync(fd)
            finally:
                os.close(fd)
    systemctl("daemon-reload")
    phase(work, state, "rolled-back")
    return {"phase": "rolled-back", "services": "left stopped for operator", "data": str(output),
            "archives": "retained untouched; no snapshot restoration"}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command", choices=("prepare", "activate", "rollback", "verify"))
    parser.add_argument("--work", required=True, type=Path)
    parser.add_argument("--spec", type=Path)
    parser.add_argument("--gate", type=Path)
    args = parser.parse_args()
    try:
        require(sys.platform == "linux" and os.geteuid() == 0, "local Linux root required")
        os.umask(0o077)
        work = safe_path(str(args.work))
        import fcntl
        # A single local installer lock also covers prepare in a different workdir.
        with open("/run/event-store-canary-104.lock", "a") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            if args.command == "prepare":
                require(args.spec, "--spec required")
                result = prepare(args.spec, work)
            else:
                plan, state = load(work)
                if args.command == "verify":
                    require(state["phase"] == "active", "not active")
                    require(read_json(plan["spec"]["identity"])["machineCode"] == IDENTITY, "wrong identity")
                    try:
                        result = verify(plan, work)
                    except (RuntimeError, OSError, ValueError, subprocess.SubprocessError):
                        phase(work, state, "needs-rollback")
                        raise
                else:
                    require(args.gate, "--gate required")
                    result = globals()[args.command](plan, state, work, args.gate)
        print(json.dumps(result, indent=2))
        return 0
    except (RuntimeError, OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        # Subprocess output may include operational secrets. Do not echo it.
        print(json.dumps({"status": "failed; preserve data and restart inhibition",
                          "recovery": "If cutover began: operator stop ONLY EventStore canary and the three original units, then run rollback --work WORK --gate GATE. Never restore snapshots.",
                          "error": str(error) if not isinstance(error, subprocess.SubprocessError) else type(error).__name__}), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
