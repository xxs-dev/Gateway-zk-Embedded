#!/usr/bin/env python3
"""Black-box delivery IPC tests for an existing Linux EventStore executable.

No build, deployment, database inspection, or third-party Python modules are used.
Run with --binary /absolute/path/to/EventStore [--sqlite-library /path/libsqlite3.so].
Use --output /path/report.json to save results, including failed runs.
Small-frame Claim rejection is covered by C++ tests: lab config has no maxFrameBytes.
Resource samples are instantaneous observations of this small test load, not peaks.
"""

import argparse
import contextlib
import datetime
import json
import os
from pathlib import Path
import platform
import re
import socket
import struct
import subprocess
import sys
import tempfile
import time
import unittest


MAX_FRAME = 256 * 1024
RPC_TIMEOUT = 5.0
STORE_ID = "delivery-ipc-test"
GENERATION = "delivery-ipc-v1"
MAIN = "main-sender"
PEER = "peer-sender"
MANAGEMENT = "management-sender"
OTHER = "other-sender"
SENDERS = [
    {"senderId": MAIN, "targetId": "main", "eventTypes": ["alarm", "change"]},
    {"senderId": PEER, "targetId": "main", "eventTypes": ["alarm", "change"]},
    {"senderId": MANAGEMENT, "targetId": "main", "eventTypes": ["management"]},
    {"senderId": OTHER, "targetId": "third", "eventTypes": ["alarm", "change"]},
]


def encode(value):
    return json.dumps(value, ensure_ascii=True, separators=(",", ":"), allow_nan=False).encode("ascii")


def envelope(operation, args):
    return {"version": "1", "storeId": STORE_ID, "configGeneration": GENERATION,
            "op": operation, "args": args}


def wire(operation, args):
    return encode(envelope(operation, args))


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise AssertionError("duplicate response field: " + key)
        result[key] = value
    return result


def read_proc_counters(path, keys, unit=None):
    values = {}
    with path.open(encoding="ascii", errors="replace") as source:
        for line in source:
            key, separator, text = line.partition(":")
            if not separator or key not in keys:
                continue
            parts = text.split()
            if len(parts) != (2 if unit else 1) or (unit and parts[1] != unit):
                raise ValueError("unexpected unit/format for " + key)
            number = int(parts[0])
            if number < 0:
                raise ValueError("negative counter for " + key)
            # smaps repeats each counter once per mapping; rollup/status/io do not.
            values[key] = values.get(key, 0) + number
    return values


def read_proc_cpu(path):
    text = path.read_text(encoding="ascii", errors="replace")
    # comm (field 2) may contain spaces or ')'; numeric fields follow its last closing ')'.
    _, separator, tail = text.rpartition(") ")
    if not separator:
        raise ValueError("invalid proc stat comm field")
    fields = tail.split()
    indices = {"userTicks": 11, "systemTicks": 12, "childrenUserTicks": 13,
               "childrenSystemTicks": 14, "startTimeTicks": 19}
    if len(fields) <= max(indices.values()):
        raise ValueError("truncated proc stat")
    values = {key: int(fields[index]) for key, index in indices.items()}
    if any(value < 0 for value in values.values()):
        raise ValueError("negative proc stat CPU counter")
    values["totalTicks"] = values["userTicks"] + values["systemTicks"]
    return values


def resource_sample(pid, profile, phase, test_name, running=True, proc_root=Path("/proc")):
    started = time.monotonic()
    sample = {"test": test_name, "profile": profile, "phase": phase, "pid": pid,
              "sampledAt": utc_now(), "processRunning": running,
              "rssKiB": None, "pssKiB": None, "rssSource": None, "pssSource": None,
              "statusKiB": {}, "smapsKiB": {}, "cpu": {}, "io": {},
              "sources": {}, "unavailable": {}}
    sources, unavailable = sample["sources"], sample["unavailable"]
    root = proc_root / str(pid)

    def attempt(name, read):
        try:
            if not running:
                raise OSError("child already exited; PID is not sampled")
            values = read()
        except (OSError, ValueError) as error:
            sources[name] = {"available": False, "reason": str(error)}
            return {}
        sources[name] = {"available": True}
        return values

    def complete(name, values, keys):
        for key in keys:
            if key not in values:
                values[key] = None
                unavailable[name + "." + key] = "source unreadable or field missing"
        return values

    status_keys = ("VmRSS", "VmSize", "RssAnon", "RssFile", "RssShmem", "VmSwap")
    sample["statusKiB"] = complete("statusKiB", attempt("status", lambda: read_proc_counters(
        root / "status", status_keys, "kB")), status_keys)
    memory_keys = ("Rss", "Pss", "Shared_Clean", "Shared_Dirty", "Private_Clean", "Private_Dirty", "Swap", "SwapPss")
    memory = attempt("smaps_rollup", lambda: read_proc_counters(root / "smaps_rollup", memory_keys, "kB"))
    memory_source = "smaps_rollup"
    if "Rss" not in memory or "Pss" not in memory:
        fallback = attempt("smaps", lambda: read_proc_counters(root / "smaps", memory_keys, "kB"))
        if "Rss" in fallback and "Pss" in fallback:
            memory, memory_source = fallback, "smaps"
    sample["smapsKiB"] = complete("smapsKiB", memory, memory_keys)
    if memory["Pss"] is not None:
        sample["pssKiB"], sample["pssSource"] = memory["Pss"], memory_source
    else:
        unavailable["pssKiB"] = "PSS unavailable from smaps_rollup and smaps; not estimated"
    if memory["Rss"] is not None:
        sample["rssKiB"], sample["rssSource"] = memory["Rss"], memory_source
    elif sample["statusKiB"]["VmRSS"] is not None:
        sample["rssKiB"], sample["rssSource"] = sample["statusKiB"]["VmRSS"], "status.VmRSS"
    else:
        unavailable["rssKiB"] = "RSS unavailable from smaps_rollup, smaps and status"
    cpu_keys = ("userTicks", "systemTicks", "childrenUserTicks", "childrenSystemTicks", "startTimeTicks", "totalTicks")
    sample["cpu"] = complete("cpu", attempt("stat", lambda: read_proc_cpu(root / "stat")), cpu_keys)
    try:
        ticks = os.sysconf("SC_CLK_TCK")
        if ticks <= 0:
            raise ValueError("nonpositive SC_CLK_TCK")
        sample["cpu"]["clockTicksPerSecond"] = ticks
    except (OSError, ValueError) as error:
        sample["cpu"]["clockTicksPerSecond"] = None
        unavailable["cpu.clockTicksPerSecond"] = str(error)
    io_keys = ("rchar", "wchar", "syscr", "syscw", "read_bytes", "write_bytes", "cancelled_write_bytes")
    sample["io"] = complete("io", attempt("io", lambda: read_proc_counters(root / "io", io_keys)), io_keys)
    sample["durationSeconds"] = round(time.monotonic() - started, 6)
    return sample


class Reply:
    def __init__(self, raw):
        self.raw = raw
        self.value = json.loads(raw.decode("utf-8"), object_pairs_hook=unique_object)
        if not isinstance(self.value, dict):
            raise AssertionError("RPC response must be a JSON object")


class LabStore:
    def __init__(self, binary, sqlite_library, profile, resource_samples=None, test_name=""):
        self.binary = binary
        self.sqlite_library = sqlite_library
        self.profile = profile
        self.max_frame = MAX_FRAME
        self.process = None
        self.resource_samples = [] if resource_samples is None else resource_samples
        self.test_name = test_name

    def __enter__(self):
        # Keep sun_path below the runtime's 100-byte config limit, regardless of TMPDIR.
        self.temp = tempfile.TemporaryDirectory(prefix="es-ipc-", dir="/tmp")
        self.directory = Path(self.temp.name)
        self.socket_path = str(self.directory / "events.sock")
        self.config_path = self.directory / "lab.json"
        config = {
            "laboratoryOnly": True, "storeId": STORE_ID, "configGeneration": GENERATION,
            "databasePath": str(self.directory / "events.db"), "socketPath": self.socket_path,
            "sqliteLibraryPath": self.sqlite_library, "storageProfile": self.profile,
            "producers": ["p0"], "senders": SENDERS,
        }
        try:
            self.config_path.write_bytes(encode(config))
            self.log = tempfile.TemporaryFile(mode="w+b", dir=self.directory)
            self.start()
            return self
        except BaseException:
            self.close()
            raise

    def __exit__(self, kind, value, traceback):
        diagnostics = ""
        try:
            diagnostics = self.diagnostics() if kind is not None else ""
        finally:
            try:
                self.close()
            finally:
                if diagnostics:
                    print("\nEventStore log:\n" + diagnostics, file=sys.stderr)

    def diagnostics(self):
        self.log.seek(0, os.SEEK_END)
        self.log.seek(max(0, self.log.tell() - 8192))
        return self.log.read().decode("utf-8", errors="replace")

    def sample_resources(self, phase):
        process = self.process
        if process is None:
            return
        try:
            sample = resource_sample(process.pid, self.profile, phase, self.test_name,
                                     running=process.poll() is None)
        except Exception as error:
            # Observability failures must not skip process termination or alter RPC test results.
            sample = {"test": self.test_name, "profile": self.profile, "phase": phase,
                      "pid": process.pid, "sampledAt": utc_now(), "rssKiB": None, "pssKiB": None,
                      "unavailable": {"collection": type(error).__name__ + ": " + str(error)}}
        self.resource_samples.append(sample)

    def start(self):
        self.process = subprocess.Popen(
            [self.binary, "--lab-config", str(self.config_path)],
            cwd=str(self.directory), stdin=subprocess.DEVNULL,
            stdout=self.log, stderr=subprocess.STDOUT,
        )
        deadline = time.monotonic() + 10.0
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                raise AssertionError("EventStore exited during startup: " + self.diagnostics())
            try:
                reply = self.call("Hello", {}, timeout=0.25)
            except OSError:
                time.sleep(0.025)
                continue
            if reply.value.get("ok") is not True or reply.value.get("laboratoryOnly") is not True:
                raise AssertionError("unexpected Hello response: " + repr(reply.value))
            expected_mode = "wal" if self.profile == "wal-full" else "delete"
            if reply.value.get("journalMode") != expected_mode:
                raise AssertionError("requested storage profile was not applied")
            self.sample_resources("after_start")
            return
        raise AssertionError("EventStore readiness deadline exceeded: " + self.diagnostics())

    def stop(self):
        if self.process is None:
            return
        self.sample_resources("before_stop")
        process, self.process = self.process, None
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5.0)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5.0)

    def restart(self):
        self.stop()
        self.start()

    def close(self):
        try:
            self.stop()
        finally:
            try:
                if hasattr(self, "log"):
                    self.log.close()
            finally:
                self.temp.cleanup()

    @contextlib.contextmanager
    def submitted(self, body, timeout=RPC_TIMEOUT):
        if not 0 < len(body) <= self.max_frame:
            raise AssertionError("test request exceeds configured frame bound")
        deadline = time.monotonic() + timeout
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as peer:
            peer.settimeout(timeout)
            peer.connect(self.socket_path)
            peer.settimeout(max(0.001, deadline - time.monotonic()))
            peer.sendall(struct.pack("!I", len(body)) + body)
            yield peer, deadline

    @staticmethod
    def receive_exact(peer, count, deadline):
        chunks = []
        while count:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("RPC response absolute deadline exceeded")
            peer.settimeout(remaining)
            chunk = peer.recv(count)
            if not chunk:
                raise AssertionError("EventStore closed an incomplete response frame")
            chunks.append(chunk)
            count -= len(chunk)
        return b"".join(chunks)

    def exchange(self, body, timeout=RPC_TIMEOUT):
        with self.submitted(body, timeout) as (peer, deadline):
            size = struct.unpack("!I", self.receive_exact(peer, 4, deadline))[0]
            if not 0 < size <= self.max_frame:
                raise AssertionError("response frame exceeds configured bound: " + str(size))
            return Reply(self.receive_exact(peer, size, deadline))

    def call(self, operation, args, timeout=RPC_TIMEOUT):
        return self.exchange(wire(operation, args), timeout)

    def wait_receipt(self, sender, sequence):
        deadline = time.monotonic() + RPC_TIMEOUT
        last = None
        while time.monotonic() < deadline:
            last = self.call("GetDeliveryReceipt", {"senderId": sender},
                             timeout=max(0.001, deadline - time.monotonic()))
            value = last.value
            if value.get("ok") is not True:
                raise AssertionError("receipt lookup failed: " + repr(value))
            if value.get("status") != "IN_PROGRESS":
                actual = value.get("sequence")
                if not isinstance(actual, str) or not re.fullmatch(r"0|[1-9][0-9]*", actual):
                    raise AssertionError("receipt sequence is not a canonical decimal string")
                if int(actual) >= sequence:
                    return last
            time.sleep(0.01)
        raise AssertionError("receipt did not reach sequence {}: {}".format(
            sequence, repr(last.value if last else None)[:512]))


def event(event_id, target="main", event_type="change", payload='{"value":1}', topic="test/events"):
    return {"eventId": event_id, "targetId": target, "eventType": event_type,
            "topic": topic, "payload": payload, "eventTs": str(int(time.time() * 1000))}


def claim_args(sender=MAIN, sequence=1, limit=16, max_bytes=32768):
    return {"senderId": sender, "epoch": "1", "sequence": str(sequence),
            "limit": str(limit), "maxBytes": str(max_bytes), "leaseMs": "30000"}


def finish_args(claim, sender=MAIN, sequence=2):
    return {"senderId": sender, "epoch": "1", "sequence": str(sequence),
            "claimToken": claim.value["claimToken"],
            "items": [{"id": item["id"], "eventId": item["eventId"]}
                      for item in claim.value["messages"]]}


class DeliveryIpcTests(unittest.TestCase):
    def __str__(self):
        return "{} [{}]".format(self._testMethodName, self.profile)

    def server(self):
        return LabStore(self.binary, self.sqlite_library, self.profile, self.resource_samples, self._testMethodName)

    def success(self, reply):
        self.assertIs(reply.value.get("ok"), True, repr(reply.value)[:512])
        return reply.value

    def delivery(self, reply, sender, sequence, status, epoch="1"):
        value = self.success(reply)
        self.assertEqual(value.get("senderId"), sender)
        self.assertEqual(value.get("epoch"), epoch)
        self.assertEqual(value.get("sequence"), str(sequence))
        self.assertEqual(value.get("status"), status)
        return value

    def register(self, store, sender=MAIN):
        args = {"senderId": sender, "sessionId": "session-" + sender, "expectedEpoch": "0"}
        reply = store.call("RegisterSender", args)
        self.delivery(reply, sender, 0, "REGISTERED")
        self.assertEqual(store.call("RegisterSender", args).raw, reply.raw)
        return reply

    def append(self, store, events, sequence=1):
        if sequence == 1:
            registered = self.success(store.call("RegisterProducer", {
                "producerId": "p0", "sessionId": "producer-session", "expectedEpoch": "0"}))
            self.assertEqual(registered.get("epoch"), "1")
        response = self.success(store.call("AppendEventsAndStates", {
            "producerId": "p0", "epoch": "1", "sequence": str(sequence), "events": events, "states": []}))
        self.assertEqual(response.get("sequence"), str(sequence))

    def messages(self, reply, expected):
        actual = reply.value.get("messages")
        self.assertIsInstance(actual, list)
        self.assertEqual(len(actual), len(expected))
        indexed = {item["eventId"]: item for item in actual}
        self.assertEqual(set(indexed), {item["eventId"] for item in expected})
        ids = set()
        for source in expected:
            item = indexed[source["eventId"]]
            self.assertIsInstance(item.get("id"), str)
            self.assertRegex(item["id"], r"^[1-9][0-9]*$")
            self.assertNotIn(item["id"], ids)
            ids.add(item["id"])
            for key in ("eventId", "eventType", "topic", "payload", "eventTs"):
                self.assertEqual(item.get(key), source[key], "message field changed: " + key)
        if actual:
            self.assertIsInstance(reply.value.get("claimToken"), str)
            self.assertTrue(0 < len(reply.value["claimToken"].encode("utf-8")) <= 256)
            self.assertRegex(reply.value.get("bootId", ""),
                             r"^[0-9a-fA-F]{8}(-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}$")
            self.assertIsInstance(reply.value.get("leaseUntilMs"), str)
            self.assertRegex(reply.value["leaseUntilMs"], r"^[1-9][0-9]*$")

    def finished(self, reply, args, statuses):
        value = self.delivery(reply, args["senderId"], int(args["sequence"]), "FINISHED")
        results = value.get("results")
        self.assertIsInstance(results, list)
        self.assertEqual(len(results), len(args["items"]))
        self.assertEqual({(row["id"], row["eventId"]) for row in results},
                         {(row["id"], row["eventId"]) for row in args["items"]})
        for result in results:
            self.assertIn(result.get("status"), statuses)

    def pending(self, store, target="main"):
        value = self.success(store.call("GetStats", {"targetId": target})).get("pendingCount")
        self.assertIsInstance(value, str)
        self.assertRegex(value, r"^(0|[1-9][0-9]*)$")
        return int(value)

    def rejected(self, reply):
        self.assertIs(reply.value.get("ok"), False)
        self.assertEqual(reply.value.get("code"), "REJECTED", repr(reply.value)[:512])
        self.assertEqual(reply.value.get("outcome"), "not_queued")

    def test_claim_lost_response_and_restart(self):
        with self.server() as store:
            self.delivery(store.call("GetDeliveryReceipt", {"senderId": MAIN}),
                          MAIN, 0, "UNREGISTERED", epoch="0")
            self.register(store)
            events = [event("lost-claim-1", payload='{"text":"\u4e2d\u6587","quote":"\\\""}'),
                      event("lost-claim-2", event_type="alarm")]
            self.append(store, events)
            body = wire("ClaimBatch", claim_args())
            # Do not recv or close early: receipt polling proves the request reached COMMIT.
            with store.submitted(body):
                recovered = store.wait_receipt(MAIN, 1)
                self.delivery(recovered, MAIN, 1, "CLAIMED")
                self.messages(recovered, events)
            self.assertEqual(store.call("GetDeliveryReceipt", {"senderId": MAIN}).raw, recovered.raw)
            self.assertEqual(store.exchange(body).raw, recovered.raw)
            self.assertEqual(self.pending(store), 2)
            store.restart()
            self.assertEqual(store.call("GetDeliveryReceipt", {"senderId": MAIN}).raw, recovered.raw)
            self.assertEqual(store.exchange(body).raw, recovered.raw)
            self.messages(recovered, events)

    def test_ack_lost_response_and_same_sequence_retry(self):
        with self.server() as store:
            self.register(store)
            events = [event("lost-ack-1"), event("lost-ack-2")]
            self.append(store, events)
            batch = store.call("ClaimBatch", claim_args())
            self.delivery(batch, MAIN, 1, "CLAIMED")
            self.messages(batch, events)
            args = finish_args(batch)
            body = wire("AckBatch", args)
            with store.submitted(body):
                recovered = store.wait_receipt(MAIN, 2)
                self.finished(recovered, args, {"APPLIED"})
            self.assertEqual(store.call("GetDeliveryReceipt", {"senderId": MAIN}).raw, recovered.raw)
            self.assertEqual(store.exchange(body).raw, recovered.raw)
            self.assertEqual(self.pending(store), 0)
            store.restart()
            self.assertEqual(store.call("GetDeliveryReceipt", {"senderId": MAIN}).raw, recovered.raw)
            self.assertEqual(store.exchange(body).raw, recovered.raw)
            empty = store.call("ClaimBatch", claim_args(sequence=3))
            self.delivery(empty, MAIN, 3, "EMPTY")
            self.messages(empty, [])

    def test_scope_isolation_and_release(self):
        with self.server() as store:
            for sender in (MAIN, PEER, MANAGEMENT, OTHER):
                self.register(store, sender)
            main_events = [event("main-change"), event("main-alarm", event_type="alarm")]
            management_events = [event("main-management", event_type="management")]
            other_events = [event("third-change", target="third"),
                            event("third-alarm", target="third", event_type="alarm")]
            self.append(store, main_events + management_events + other_events)
            main_claim = store.call("ClaimBatch", claim_args())
            self.delivery(main_claim, MAIN, 1, "CLAIMED")
            self.messages(main_claim, main_events)
            peer_claim = store.call("ClaimBatch", claim_args(PEER))
            self.delivery(peer_claim, PEER, 1, "EMPTY")
            self.messages(peer_claim, [])
            management_claim = store.call("ClaimBatch", claim_args(MANAGEMENT))
            self.delivery(management_claim, MANAGEMENT, 1, "CLAIMED")
            self.messages(management_claim, management_events)
            other_claim = store.call("ClaimBatch", claim_args(OTHER))
            self.delivery(other_claim, OTHER, 1, "CLAIMED")
            self.messages(other_claim, other_events)

            # A valid token cannot authorize another sender, nor foreign items in one's own batch.
            stolen = finish_args(main_claim, sender=PEER)
            self.finished(store.call("AckBatch", stolen), stolen, {"REVOKED", "NOT_CLAIMED"})
            foreign = finish_args(management_claim, sender=MANAGEMENT)
            foreign["items"] = finish_args(main_claim)["items"]
            self.finished(store.call("AckBatch", foreign), foreign, {"NOT_CLAIMED"})
            self.assertEqual(self.pending(store), 3)
            self.assertEqual(self.pending(store, "third"), 2)

            release_args = finish_args(main_claim)
            released = store.call("ReleaseBatch", release_args)
            self.finished(released, release_args, {"APPLIED"})
            self.assertEqual(store.call("ReleaseBatch", release_args).raw, released.raw)
            reclaimed = store.call("ClaimBatch", claim_args(PEER, sequence=3))
            self.delivery(reclaimed, PEER, 3, "CLAIMED")
            self.messages(reclaimed, main_events)
            self.assertEqual({row["id"] for row in reclaimed.value["messages"]},
                             {row["id"] for row in main_claim.value["messages"]})
            ack = finish_args(reclaimed, sender=PEER, sequence=4)
            self.finished(store.call("AckBatch", ack), ack, {"APPLIED"})
            self.assertEqual(self.pending(store), 1)
            self.assertEqual(self.pending(store, "third"), 2)

    def test_malformed_and_unknown_fields_do_not_queue(self):
        with self.server() as store:
            registered = self.register(store)
            events = [event("after-rejections")]
            self.append(store, events)
            claim = claim_args()
            finish = {"senderId": MAIN, "epoch": "1", "sequence": "1",
                      "claimToken": "test-token", "items": [{"id": "1", "eventId": "item"}]}
            operations = {
                "RegisterSender": {"senderId": MAIN, "sessionId": "replacement", "expectedEpoch": "1"},
                "GetDeliveryReceipt": {"senderId": MAIN}, "ClaimBatch": claim,
                "AckBatch": finish, "ReleaseBatch": finish,
            }
            cases = [("broken JSON", b"{"), ("non-object root", b"[]"), ("invalid UTF-8", b"\xff")]
            for operation, args in operations.items():
                cases.append((operation + " unknown argument", wire(operation, dict(args, unknown=True))))
                cases.append((operation + " unconfigured sender", wire(operation, dict(args, senderId="absent"))))
                cases.append((operation + " empty sender", wire(operation, dict(args, senderId=""))))
                cases.append((operation + " long sender", wire(operation, dict(args, senderId="x" * 97))))
                root = envelope(operation, args)
                root["unknown"] = "field"
                cases.append((operation + " unknown envelope field", encode(root)))
                duplicate = wire(operation, args).replace(
                    b'"senderId":', b'"senderId":"absent","senderId":', 1)
                cases.append((operation + " duplicate argument", duplicate))
            for field in claim:
                missing = dict(claim)
                del missing[field]
                cases.append(("missing " + field, wire("ClaimBatch", missing)))
            for field in ("epoch", "sequence", "limit", "maxBytes", "leaseMs"):
                for invalid in (1, True, None, "", "01", "+1", "-0", " 1", "1 ", "1.0", "1e0",
                                "9223372036854775808"):
                    cases.append((field + "=" + repr(invalid), wire("ClaimBatch", dict(claim, **{field: invalid}))))
            for field, invalid in (("epoch", "0"), ("sequence", "0"), ("limit", "0"), ("limit", "17"),
                                   ("maxBytes", "0"), ("maxBytes", "32769"), ("leaseMs", "99"), ("leaseMs", "30001")):
                cases.append((field + " out of bounds " + invalid, wire("ClaimBatch", dict(claim, **{field: invalid}))))
            for operation in ("AckBatch", "ReleaseBatch"):
                for invalid in ([], finish["items"] * 17, {}, [None], [{}]):
                    cases.append((operation + " invalid items " + repr(invalid)[:80],
                                  wire(operation, dict(finish, items=invalid))))
                for invalid in ("", "x" * 257, "token\x00", 1):
                    cases.append((operation + " invalid token " + repr(invalid)[:80],
                                  wire(operation, dict(finish, claimToken=invalid))))
                for field, invalid in (("id", 1), ("id", "0"), ("id", "01"),
                                       ("eventId", ""), ("eventId", "x" * 257), ("eventId", "event\x00"),
                                       ("unknown", "field")):
                    item = dict(finish["items"][0], **{field: invalid})
                    cases.append((operation + " invalid item " + field + "=" + repr(invalid)[:80],
                                  wire(operation, dict(finish, items=[item]))))
            for field, invalid in (("sessionId", ""), ("sessionId", "x" * 97), ("expectedEpoch", 1),
                                   ("expectedEpoch", "01"), ("expectedEpoch", "-1")):
                args = dict(operations["RegisterSender"], **{field: invalid})
                cases.append(("invalid registration " + field, wire("RegisterSender", args)))
            for field, invalid in (("version", 1), ("version", "01"), ("storeId", "wrong-store"),
                                   ("configGeneration", "wrong-generation"), ("op", "UnknownOperation"), ("args", [])):
                root = envelope("ClaimBatch", claim)
                root[field] = invalid
                cases.append(("invalid envelope " + field, encode(root)))
            for name, body in cases:
                with self.subTest(case=name):
                    self.rejected(store.exchange(body))
            self.assertEqual(store.call("GetDeliveryReceipt", {"senderId": MAIN}).raw, registered.raw)
            self.assertEqual(self.pending(store), 1)
            batch = store.call("ClaimBatch", claim)
            self.delivery(batch, MAIN, 1, "CLAIMED")
            self.messages(batch, events)

    def test_maximum_escaped_payload_budget(self):
        with self.server() as store:
            registered = self.register(store)
            # Every topic/payload byte expands to six JSON bytes; ids also hit 256 bytes each.
            events = [event("{:02d}".format(index) + "\x03" * 254,
                            topic="\x02" * 64, payload="\x01" * (2048 - 64))
                      for index in range(16)]
            self.assertEqual(sum(len(item["topic"].encode("utf-8")) + len(item["payload"].encode("utf-8"))
                                 for item in events), 32768)
            self.append(store, events)
            self.rejected(store.call("ClaimBatch", claim_args(max_bytes=32769)))
            self.assertEqual(store.call("GetDeliveryReceipt", {"senderId": MAIN}).raw, registered.raw)
            body = wire("ClaimBatch", claim_args(max_bytes=32768))
            with store.submitted(body):
                recovered = store.wait_receipt(MAIN, 1)
                self.delivery(recovered, MAIN, 1, "CLAIMED")
                self.messages(recovered, events)
            self.assertGreater(len(recovered.raw), 6 * 32768)
            self.assertLessEqual(len(recovered.raw), MAX_FRAME)
            self.assertEqual(store.exchange(body).raw, recovered.raw)
            self.assertEqual(store.call("GetDeliveryReceipt", {"senderId": MAIN}).raw, recovered.raw)
            args = finish_args(recovered)
            self.finished(store.call("AckBatch", args), args, {"APPLIED"})
            self.assertEqual(self.pending(store), 0)


class ReportingResult(unittest.TextTestResult):
    def __init__(self, stream, descriptions, verbosity):
        super().__init__(stream, descriptions, verbosity)
        self.records = []

    def startTest(self, test):
        super().startTest(test)
        self.started = time.monotonic()
        self.current = {"name": test._testMethodName, "profile": test.profile,
                        "status": "passed", "durationSeconds": 0.0, "details": []}
        self.records.append(self.current)

    def stopTest(self, test):
        self.current["durationSeconds"] = round(time.monotonic() - self.started, 6)
        super().stopTest(test)

    def record_problem(self, test, err, status):
        if self.current["status"] != "error":
            self.current["status"] = status
        self.current["details"].append(self._exc_info_to_string(err, test))

    def addFailure(self, test, err):
        super().addFailure(test, err)
        self.record_problem(test, err, "failed")

    def addError(self, test, err):
        super().addError(test, err)
        self.record_problem(test, err, "error")

    def addSkip(self, test, reason):
        super().addSkip(test, reason)
        self.current["status"] = "skipped"
        self.current["details"].append(reason)

    def addSubTest(self, test, subtest, err):
        super().addSubTest(test, subtest, err)
        if err is not None:
            status = "failed" if issubclass(err[0], test.failureException) else "error"
            self.record_problem(subtest, err, status)
            self.current["details"][-1] = str(subtest) + "\n" + self.current["details"][-1]


def utc_now():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def save_report(path, report):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=str(path.parent),
                                         prefix=path.name + ".", suffix=".tmp", delete=False) as output:
            temporary = output.name
            json.dump(report, output, ensure_ascii=True, indent=2, allow_nan=False)
            output.write("\n")
        os.replace(temporary, str(path))
        temporary = None
    finally:
        if temporary is not None:
            os.unlink(temporary)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, help="existing Linux EventStore executable")
    parser.add_argument("--sqlite-library", default="", help="SQLite shared library; default: binary's normal lookup")
    parser.add_argument("--profile", choices=("both", "delete-full", "wal-full"), default="both")
    parser.add_argument("--output", type=Path, help="save a JSON report, including test failures")
    args = parser.parse_args()
    if os.name != "posix" or not hasattr(socket, "AF_UNIX"):
        parser.error("run this script and the Linux binary together under WSL or on the Linux host")
    binary = Path(args.binary).expanduser().resolve()
    if not binary.is_file() or not os.access(str(binary), os.X_OK):
        parser.error("--binary must name an existing executable file")
    sqlite_library = ""
    if args.sqlite_library:
        library = Path(args.sqlite_library).expanduser().resolve()
        if not library.is_file():
            parser.error("--sqlite-library must name an existing shared library")
        sqlite_library = str(library)
    output = args.output.expanduser().resolve() if args.output else None
    if output in (binary, Path(__file__).resolve()) or (output and str(output) == sqlite_library):
        parser.error("--output must not overwrite the binary, SQLite library, or test script")
    profiles = ("delete-full", "wal-full") if args.profile == "both" else (args.profile,)
    suite = unittest.TestSuite()
    resource_samples = []
    for profile in profiles:
        for name in unittest.defaultTestLoader.getTestCaseNames(DeliveryIpcTests):
            case = DeliveryIpcTests(name)
            case.binary, case.sqlite_library, case.profile = str(binary), sqlite_library, profile
            case.resource_samples = resource_samples
            suite.addTest(case)
    started_at, started = utc_now(), time.monotonic()
    result = unittest.TextTestRunner(verbosity=2, resultclass=ReportingResult).run(suite)
    counts = {status: sum(record["status"] == status for record in result.records)
              for status in ("passed", "failed", "error", "skipped")}
    report = {
        "schemaVersion": 1, "suite": "event-store-delivery-ipc", "success": result.wasSuccessful(),
        "startedAt": started_at, "finishedAt": utc_now(),
        "durationSeconds": round(time.monotonic() - started, 6),
        "binary": str(binary), "sqliteLibrary": sqlite_library, "profiles": list(profiles),
        "platform": platform.platform(), "architecture": platform.machine(), "pythonVersion": platform.python_version(),
        "summary": dict(counts, testsRun=result.testsRun), "tests": result.records,
        "resourceSamples": resource_samples,
        "resourceObservation": {
            "kind": "point-in-time", "phases": ["after_start", "before_stop"], "memoryUnit": "KiB",
            "note": "Instantaneous observations of a small test load, not peak or sustained resource usage. "
                    "Proc files are read sequentially, not atomically. CPU and I/O counters are cumulative per process; "
                    "cpu.totalTicks excludes child CPU time. Missing measurements are null with unavailable reasons.",
        },
        "externalCoverage": [{"case": "ClaimBatch rejection with maxFrameBytes <= 65536",
                              "owner": "C++ tests", "executedByThisScript": False,
                              "reason": "lab config does not expose maxFrameBytes"}],
    }
    if output:
        try:
            save_report(output, report)
        except OSError as error:
            print("cannot save JSON report: " + str(error), file=sys.stderr)
            return 2
        print("JSON report: " + str(output))
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
