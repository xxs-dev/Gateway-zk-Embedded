"""Small deterministic installer tests; all systemd/OTA/production calls mocked."""

import copy
import contextlib
import json
import os
from pathlib import Path
import struct
import tempfile
import time
import unittest
from unittest.mock import patch

import event_store_canary_deploy as deploy


class CanaryTests(unittest.TestCase):
    def test_save_preserves_literal_utf8_for_legacy_config_parser(self):
        # ASCII source keeps this fixture portable; output must contain UTF-8.
        label = "\u4e2d\u6587\u544a\u8b66"
        value = {label: {"name": label, "quoted": '"' + label + '"',
                         "path": "C:\\fixture", "lines": label + "\nnext"}}
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "app.json"
            deploy.save(path, value)
            raw = path.read_bytes()
            self.assertIn(label.encode("utf-8"), raw)
            self.assertNotIn(b"\\u", raw)
            self.assertFalse(raw.startswith(b"\xef\xbb\xbf"))
            self.assertEqual(json.loads(raw.decode("utf-8")), value)

    def test_start_gate_is_optional_and_fixed_absolute_path_only(self):
        self.assertIsNone(deploy.check_start_gate({}))
        self.assertEqual(deploy.START_GATE, "/run/event-store-cutover-104.ready")
        for value in (None, "", "event-store-cutover-104.ready", "/tmp/ready",
                      "/run/../run/event-store-cutover-104.ready"):
            with self.subTest(value=value), self.assertRaisesRegex(RuntimeError, "start_gate must be"):
                deploy.check_start_gate({"start_gate": value})

    @unittest.skipUnless(os.name == "posix", "Linux gate filesystem contract")
    def test_start_gate_preconditions_and_atomic_no_clobber(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            gate = root / "ready"
            spec = {"start_gate": str(gate)}
            with patch.object(deploy, "START_GATE", str(gate)):
                deploy.check_start_gate(spec)
                with self.assertRaisesRegex(RuntimeError, "start_gate missing"):
                    deploy.check_start_gate(spec, present=True)
                # A racing creator must not be overwritten by atomic publication.
                link = os.link
                def race(source, target):
                    Path(target).write_bytes(b"other-owner")
                    link(source, target)
                with patch.object(deploy.os, "link", side_effect=race):
                    with self.assertRaises(FileExistsError):
                        deploy.publish_start_gate(spec)
                self.assertEqual(gate.read_bytes(), b"other-owner")
                self.assertEqual(list(root.iterdir()), [gate])
                spec_path = root / "spec.json"
                deploy.save(spec_path, spec)
                with self.assertRaisesRegex(RuntimeError, "start_gate must be absent"):
                    deploy.prepare(spec_path, root / "work")
                with self.assertRaisesRegex(RuntimeError, "start_gate must be absent"):
                    deploy.activate({"spec": spec}, {"phase": "prepared"}, root, root / "evidence")
                gate.unlink()
                deploy.publish_start_gate(spec)
                self.assertEqual(deploy.check_start_gate(spec, present=True), gate)
                self.assertIn(b"Hello verified", gate.read_bytes())
                self.assertEqual(gate.stat().st_mode & 0o777, 0o600)
                gate.unlink()
                gate.symlink_to(root / "missing")
                with self.assertRaises(RuntimeError):
                    deploy.check_start_gate(spec)

    def test_verify_allows_wrapper_to_remove_activation_gate(self):
        plan = {"spec": {"start_gate": deploy.START_GATE, "services": []}, "configs": {}, "units": {}}
        with patch.object(deploy, "check_start_gate", side_effect=AssertionError("not a long-term gate")), \
                patch.object(deploy, "check_sources"), patch.object(deploy, "effective_commands") as effective, \
                patch.object(deploy, "systemctl", return_value="active"), \
                patch.object(deploy, "read_json", return_value={}), patch.object(deploy, "hello", return_value={"ok": True}):
            self.assertEqual(deploy.verify(plan, Path("/review"))["phase"], "active")
            effective.assert_called_once_with(plan, Path("/review"), running=True)

    @unittest.skipUnless(os.name == "posix", "Linux activation filesystem contract")
    def test_start_gate_published_after_writes_and_hello_before_clients(self):
        for ready in (True, False):
            with self.subTest(hello_ready=ready), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                gate = root / "ready"
                original, candidate, live = (root / name for name in ("original", "candidate", "live"))
                original.write_bytes(b"original")
                live.write_bytes(b"original")
                candidate.write_bytes(b"candidate")
                units = [prefix + "@mqtt-service.service" for prefix in deploy.BINARIES.values()]
                spec = {key: str(root / key) for key in
                        ("source", "archive_source", "history_source", "baseline", "data")}
                spec.update(start_gate=str(gate), services=[{"unit": u} for u in units])
                plan = {"spec": spec, "configs": {str(live): {"original": str(original), "activated": str(candidate)}},
                        "units": {str(root / "unit.conf"): "fixture unit"}, "original_units": {}}
                state = {"phase": "prepared"}
                deploy.save(root / "lab.json", {})
                deploy.save(root / "state.json", state)
                calls = []
                def ctl(*args):
                    if "start" in args:
                        unit = args[-1]
                        self.assertEqual(gate.exists(), unit != deploy.STORE_UNIT)
                        calls.append(unit)
                    return ""
                def hello(lab):
                    self.assertFalse(gate.exists())
                    self.assertEqual(live.read_bytes(), candidate.read_bytes())
                    self.assertEqual((root / "unit.conf").read_text(), "fixture unit")
                    calls.append("Hello")
                    if not ready:
                        raise RuntimeError("fixture readiness failure")
                def verify(plan, work):
                    deploy.check_start_gate(plan["spec"], present=True)
                    return {"phase": "active"}
                with contextlib.ExitStack() as stack:
                    stack.enter_context(patch.object(deploy, "START_GATE", str(gate)))
                    for name in ("gate_check", "stopped", "no_accessors", "migration_evidence", "check_sources",
                                 "capacity", "effective_commands"):
                        stack.enter_context(patch.object(deploy, name))
                    stack.enter_context(patch.object(deploy, "systemctl", side_effect=ctl))
                    stack.enter_context(patch.object(deploy, "hello", side_effect=hello))
                    stack.enter_context(patch.object(deploy, "verify", side_effect=verify))
                    stack.enter_context(patch.object(deploy.time, "monotonic", side_effect=[0, 61]))
                    if ready:
                        deploy.activate(plan, state, root, root / "evidence")
                        self.assertEqual(calls, [deploy.STORE_UNIT, "Hello"] + units)
                        self.assertEqual(state["phase"], "active")
                    else:
                        with self.assertRaisesRegex(RuntimeError, "readiness failed"):
                            deploy.activate(plan, state, root, root / "evidence")
                        self.assertFalse(gate.exists())
                        self.assertEqual(calls, [deploy.STORE_UNIT, "Hello"])

    def test_patch_preserves_credentials_policy_and_only_allowed_paths(self):
        original = {"mqtt": {"password": "fixture-only", "offlineBuffer": {
            "eventOutbox": {"sqlitePath": "/old/events", "retentionMonths": 12}}},
            "alarmStore": {"sqlitePath": "/old/history", "enabled": True},
            "policy": {"dispatch": [1, 2, 3]}, "unknownFutureField": {"keep": True}}
        frozen = copy.deepcopy(original)
        result = deploy.patched(original, {"eventStore": {"backend": "ipc-lab"}}, "/new/events", "/new/history")
        self.assertEqual(original, frozen)
        self.assertEqual(result["mqtt"]["password"], "fixture-only")
        self.assertEqual(result["policy"], original["policy"])
        self.assertEqual(set(deploy.diff_paths(original, result)),
                         {deploy.OUTBOX, deploy.ALARM, ("eventStore",)})
        with self.assertRaises(RuntimeError):
            deploy.patched(original, {"policy": {}}, "/a", "/b")

    def test_flat_release_original_config_and_only_three_units(self):
        s = {"release": "/opt/release-r05", "services": [
            {"unit": prefix + "@mqtt-service.service", "binary": binary, "config": "/opt/original.json"}
            for binary, prefix in deploy.BINARIES.items()]}
        files = deploy.unit_files(s, Path("/opt/review"))
        self.assertEqual(len(files), 4)
        text = "\n".join(files.values())
        self.assertNotIn("/bin/", text)
        self.assertNotIn("/lib/", text)
        self.assertIn("--app-config /opt/original.json", text)
        self.assertEqual(text.count("ExecStart=\n"), 3)
        self.assertEqual(text.count("Requires=" + deploy.STORE_UNIT), 3)
        self.assertIn("SendSIGKILL=no", text)
        self.assertNotIn("Restart=always", text)

    def test_rollback_uses_current_projection_and_never_original_target(self):
        p = {"spec": {"data": "/canary", "source": "/archive/9gb.db",
                      "baseline": "/archive/baseline.db", "history_source": "/archive/history.db",
                      "history_id": "legacy104"}}
        commands = deploy.rollback_commands(p, Path("/canary/recovery"))
        for command in commands:
            self.assertIn("--offline", command)
            self.assertNotIn("/archive/9gb.db", command)
            self.assertIn("/canary/recovery", command[command.index("--target") + 1].replace("\\", "/"))
        self.assertIn("rollback", commands[0])
        self.assertIn("--baseline", commands[0])
        self.assertIn("history", commands[1])
        self.assertIn("--projection", commands[1])
        self.assertEqual(commands[1][commands[1].index("--history-id") + 1], "legacy104")

    def test_stop_guard_rejects_activating_and_residual_pid(self):
        for states in (["activating"], ["inactive", "42"]):
            with patch.object(deploy, "systemctl", side_effect=states):
                with self.assertRaises(RuntimeError):
                    deploy.stopped(["event-engine@mqtt-service.service"])

    def test_effective_execstart_cannot_be_overridden_by_later_dropin(self):
        plan = {"spec": {"release": "/release", "services": []}}
        with patch.object(deploy, "systemctl", return_value=
                          "{ path=/old/EventStore ; argv[]=/old/EventStore --lab-config /review/lab.json ; }"):
            with self.assertRaisesRegex(RuntimeError, "overridden"):
                deploy.effective_commands(plan, Path("/review"))

    def test_gate_rejects_wrong_identity_and_expired_attestation(self):
        plan = {"spec": {"identity": "/identity", "ota_markers": []}}
        with patch.object(deploy, "read_json", return_value={"machineCode": "COMM202600103"}):
            with self.assertRaisesRegex(RuntimeError, "identity"):
                deploy.gate_check(plan, "/gate")
        with patch.object(deploy, "read_json", side_effect=[{"machineCode": deploy.IDENTITY},
                {"machineCode": deploy.IDENTITY, "otaIdle": True, "restartInhibited": True, "expires": time.time() - 1}]):
            with self.assertRaisesRegex(RuntimeError, "stale"):
                deploy.gate_check(plan, "/gate")

    def test_source_report_suffix_contract(self):
        with tempfile.TemporaryDirectory() as directory:
            db = Path(directory) / "source.db"
            db.write_bytes(b"fixture")
            Path(str(db) + "-shm").write_bytes(b"transient")
            Path(str(db) + "-wal").write_bytes(b"")
            self.assertEqual(deploy.file_set(db), {"": deploy.digest(db)})
            Path(str(db) + "-wal").write_bytes(b"new-event")
            self.assertEqual(set(deploy.file_set(db)), {"", "-wal"})

    def test_hello_requires_exact_private_sqlite_and_lab_marker(self):
        lab = {"socketPath": "/unused", "storeId": "integrated-lab",
               "configGeneration": "lab-v1", "storageProfile": "wal-full"}
        good = dict(lab, ok=True, laboratoryOnly=True, version="1", sqliteVersion="3.53.4",
                    synchronous="2", localJournalVersion="1", historyProjectionVersion="1", historyEnabled=True)
        for delta in ({}, {"sqliteVersion": "3.53.0"}, {"laboratoryOnly": False}):
            body = json.dumps(dict(good, **delta)).encode()
            with patch.object(deploy.socket, "socket") as factory:
                factory.return_value.__enter__.return_value.recv.side_effect = [struct.pack("!I", len(body)), body]
                if delta:
                    with self.assertRaisesRegex(RuntimeError, "Hello contract"):
                        deploy.hello(lab)
                else:
                    self.assertTrue(deploy.hello(lab)["laboratoryOnly"])

    @unittest.skipUnless(os.name == "posix", "Linux installer filesystem contract")
    def test_prepare_and_failed_recovery_preserve_inputs(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            release, data, units = (root / k for k in ("release", "data", "units"))
            for p in (release, data, units):
                p.mkdir()
            for binary in ("EventStore", *deploy.BINARIES, "libsqlite3.so"):
                (release / binary).write_bytes(b"fixture-binary")
            paths = {}
            for name in ("source", "history_source", "baseline", "archive_source"):
                paths[name] = str(root / (name + ".db"))
                Path(paths[name]).write_bytes(b"events" if name == "baseline" else b"archive")
            (data / "events.db").write_bytes(b"events")
            (data / "history.db").write_bytes(b"empty-history")
            Path(paths["baseline"]).chmod(0o444)
            Path(paths["archive_source"]).chmod(0o444)
            extraction = root / "extract.json"
            origin = deploy.archive_metadata(paths["archive_source"])
            for suffix, entry in origin.items():
                entry["sha256"] = deploy.digest(paths["archive_source"] + suffix)
            deploy.save(extraction, {"format": "event-store-workset-v1", "mode": "extract",
                "status": "OFFLINE_WORKSET_CANDIDATE", "consistent_snapshot": True,
                "source_preserved": True, "machine_code": deploy.IDENTITY,
                "source": paths["archive_source"], "target": paths["source"],
                "target_sha256": deploy.digest(paths["source"]), "origin": origin})
            identity = root / "identity.json"
            deploy.save(identity, {"machineCode": deploy.IDENTITY})
            original = root / "original.json"
            config = root / "mqtt-service.json"
            deploy.save(original, {"identityConfigFile": str(identity), "mqtt": {"password": "fixture-only",
                "offlineBuffer": {"eventOutbox": {"sqlitePath": paths["archive_source"]}}},
                "alarmStore": {"sqlitePath": paths["history_source"]},
                "policy": {"keep": True, "label": "\u4e2d\u6587\u7b56\u7565"}})
            config.write_bytes(original.read_bytes())
            lab = root / "lab-template.json"
            overlay = root / "overlay-template.json"
            deploy.save(lab, {"laboratoryOnly": True, "storeId": "integrated-lab", "configGeneration": "lab-v1",
                              "storageProfile": "wal-full", "producers": [], "senders": []})
            deploy.save(overlay, {"eventStore": {"backend": "ipc-lab", "storeId": "integrated-lab",
                                                 "configGeneration": "lab-v1", "storageProfile": "wal-full"}})
            reports = []
            for command, key, target in (("migrate", "source", data / "events.db"),
                                         ("history", "history_source", data / "history.db")):
                report = {"command": command, "publication": "published", "dry_run": False,
                          "target_sha256": deploy.digest(target), "cutover_baseline": paths["baseline"],
                          "sources": [{"path": paths[key], "files_sha256": deploy.file_set(paths[key])}]}
                if command == "history":
                    report["target"] = str(target)
                p = root / (command + ".json")
                deploy.save(p, report)
                reports.append(str(p))
            spec = dict(paths, release=str(release), data=str(data), identity=str(identity),
                        migration_report=reports[0], history_report=reports[1], history_id="legacy104", extract_manifest=str(extraction),
                        lab_template=str(lab), overlay_template=str(overlay),
                        ota_markers=["/run/gateway-health-watchdog/applying"], services=[
                            {"unit": prefix + "@mqtt-service.service", "binary": binary,
                             "config": str(config), "original": str(original)} for binary, prefix in deploy.BINARIES.items()])
            spec_path, work = root / "spec.json", root / "review"
            deploy.save(spec_path, spec)
            before = {p: deploy.digest(p) for p in (*paths.values(), config, original)}
            with patch.object(deploy, "UNIT_ROOT", units), patch.object(deploy, "capacity"), \
                    patch.object(deploy, "systemctl", return_value="fixture-unit") as ctl, \
                    patch("ctypes.CDLL") as library:
                library.return_value.sqlite3_libversion.return_value = b"3.53.4"
                result = deploy.prepare(spec_path, work)
                self.assertEqual(result["phase"], "prepared")
                self.assertTrue(all(call.args[0] == "cat" for call in ctl.call_args_list))
                self.assertEqual(list(units.iterdir()), [])
                self.assertNotIn("fixture-only", (work / "allowed-diff.json").read_text())
                self.assertEqual((work / "config-0.json").stat().st_mode & 0o777, 0o600)
                self.assertIn("\u4e2d\u6587\u7b56\u7565".encode("utf-8"), (work / "config-0.json").read_bytes())
                self.assertNotIn(b"\\u", (work / "config-0.json").read_bytes())
                plan, state = deploy.load(work)
                self.assertTrue(deploy.read_json(work / "lab.json")["laboratoryOnly"])
                with patch.object(deploy, "digest", wraps=deploy.digest) as hashes:
                    deploy.check_sources(plan)
                    self.assertNotIn(paths["archive_source"], [str(c.args[0]) for c in hashes.call_args_list])
                # Changed live config after prepare must abort before any write/start.
                config.write_bytes(b"{}")
                with patch.object(deploy, "gate_check"), patch.object(deploy, "stopped"), \
                        patch.object(deploy, "no_accessors"):
                    with self.assertRaisesRegex(RuntimeError, "config changed"):
                        deploy.activate(plan, state, work, "/gate")
                self.assertEqual(state["phase"], "prepared")
                config.write_bytes(original.read_bytes())
                state["phase"] = "activating"
                start_gate = root / "rollback-ready"
                plan["spec"]["start_gate"] = str(start_gate)
                with patch.object(deploy, "gate_check"), patch.object(deploy, "stopped"), \
                        patch.object(deploy, "no_accessors"), patch.object(deploy, "run", side_effect=RuntimeError("merge failed")):
                    with self.assertRaisesRegex(RuntimeError, "merge failed"):
                        deploy.rollback(plan, state, work, "/gate")
                self.assertEqual(state["phase"], "rolling-back")
                self.assertFalse(start_gate.exists())
                self.assertEqual({p: deploy.digest(p) for p in before}, before)
                self.assertTrue(all(call.args[0] == "cat" for call in ctl.call_args_list))
                # A successful retry publishes merges BEFORE changing app config,
                # retains the archive and leaves every service stopped.
                def merge(command):
                    target = Path(command[command.index("--target") + 1])
                    target.write_bytes(b"merged-with-canary-increments")
                    return json.dumps({"publication": "published", "target_sha256": deploy.digest(target)})
                start_gate.write_bytes(b"wrapper-owned")
                with patch.object(deploy, "gate_check"), patch.object(deploy, "stopped"), \
                        patch.object(deploy, "no_accessors"), patch.object(deploy, "run", side_effect=merge):
                    result = deploy.rollback(plan, state, work, "/gate")
                self.assertEqual(result["phase"], "rolled-back")
                self.assertEqual(start_gate.read_bytes(), b"wrapper-owned")
                self.assertEqual(deploy.read_json(config)["mqtt"]["password"], "fixture-only")
                self.assertEqual(deploy.read_json(config)["policy"], deploy.read_json(original)["policy"])
                self.assertIn("\u4e2d\u6587\u7b56\u7565".encode("utf-8"), config.read_bytes())
                self.assertNotIn(b"\\u", config.read_bytes())
                self.assertNotIn("eventStore", deploy.read_json(config))
                self.assertEqual(Path(deploy.get(deploy.read_json(config), deploy.OUTBOX)).read_bytes(),
                                 b"merged-with-canary-increments")
                for p in paths.values():
                    self.assertEqual(deploy.digest(p), before[p])
                self.assertTrue(all(c.args[0] in ("cat", "daemon-reload") for c in ctl.call_args_list))


if __name__ == "__main__":
    unittest.main()
