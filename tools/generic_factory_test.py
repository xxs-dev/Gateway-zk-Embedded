#!/usr/bin/env python3
"""Test exact generic artifacts in private mounts without executing AArch64 code."""
import argparse
import ast
import configparser
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tarfile
import unittest
import zipfile
import zlib

import install_isolation_test as harness

harness.PROTECTED = (*harness.PROTECTED, "/opt")


class GenericFactoryTest(harness.InstallIsolationTest):
    def setUp(self):
        super().setUp()
        self.env["PYTHONDONTWRITEBYTECODE"] = "1"
        harness.write(Path("/opt/isolation-sentinel"), "private-opt-only\n")
        self.before = self.snapshot()

    def init(self, name, profile="full", mode="gateway", qt="off", overlay=True, selected=None, normal=False, start=False, extra_env=None,
             candidate=None, machine="GENERIC_TEST_001"):
        candidate = candidate or CANDIDATE
        home = self.root / name
        selection = self.root / (name + ".json")
        harness.write(selection, json.dumps({"requiredDrivers": selected if selected is not None else ["ModbusRtu", "Dlt645Driver", "DioDriver"]}))
        arguments = ["--auto", "--package", candidate / "gateway-factory-defaults.tar.gz",
                     "--gateway-home", home, "--package-profile", profile, "--runtime-mode", mode,
                     "--no-mqtt-tls", "--start" if start else "--no-start", "--no-smoke",
                     "--no-direct-maintenance"]
        if machine is not None:
            arguments += ["--machine-code", machine]
        if profile == "project":
            arguments += ["--manifest", selection]
        if mode == "agc_avc" and overlay:
            arguments += ["--runtime-package", candidate / "gateway-agc-avc-runtime.tar.gz"]
        if qt == "on":
            arguments += ["--local-qt-display"]
        elif qt == "off":
            arguments += ["--no-local-qt-display"]
        result = self.run_script(str(candidate / "deploy/production-init.sh"), arguments,
                                 {"INSTALL_SYSTEMD": "1" if normal else "0", **(extra_env or {})})
        return home, result

    def copy_candidate(self, name):
        candidate = self.root / (name + "-candidate")
        shutil.copytree(CANDIDATE, candidate, ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))
        return candidate

    def mutate_archive(self, archive, member_suffix, transform):
        temporary = archive.with_name(archive.name + ".tampered")
        matches = 0
        with tarfile.open(archive) as source, tarfile.open(temporary, "w:gz") as output:
            for member in source.getmembers():
                data = source.extractfile(member).read() if member.isfile() else None
                if member.isfile() and member.name.endswith("/" + member_suffix):
                    data = transform(data)
                    member.size = len(data)
                    matches += 1
                output.addfile(member, io.BytesIO(data) if data is not None else None)
        self.assertEqual(1, matches, member_suffix)
        temporary.replace(archive)

    def assert_preflight_rejected(self, candidate, case, error, mode="gateway", machine="GENERIC_TEST_001"):
        for existing in (False, True):
            name = case + ("-existing" if existing else "-fresh")
            with self.subTest(case=name):
                home = self.root / name
                if existing:
                    harness.write(home / "config/runtime/keep", "previous runtime\n")
                    harness.write(home / "bin/keep", "previous program\n")
                    (home / "current").symlink_to("config/runtime")
                def snapshot_home():
                    return {str(path.relative_to(home)): (path.lstat().st_mode, path.lstat().st_mtime_ns,
                            os.readlink(path) if path.is_symlink() else path.read_bytes() if path.is_file() else None)
                            for path in ([home, *home.rglob("*")] if home.exists() else [])}
                before = snapshot_home()
                work = self.root / (name + "-extract")
                _, result = self.init(name, mode=mode, candidate=candidate, machine=machine, normal=True,
                                      extra_env={"INIT_WORK_DIR": str(work)})
                self.assertEqual(2, result.returncode, result.stdout)
                self.assertIn(error, result.stdout)
                self.assertEqual(before, snapshot_home(), "installation root changed before rejection")
                self.assertEqual(existing, home.exists())
                self.assertFalse(work.exists(), "temporary extraction leaked")
                self.assert_isolated()

    def test_generic_paired_deploy_tampering_rejected(self):
        candidate = self.copy_candidate("paired-deploy")
        script = candidate / "deploy/install-factory-config.sh"
        script.write_bytes(script.read_bytes() + b"\n# tampered test fixture\n")
        self.assert_preflight_rejected(candidate, "paired-deploy", "paired deploy script mismatch: install-factory-config.sh")

    def test_generic_bundle_tampering_rejected(self):
        candidate = self.copy_candidate("bundle")
        self.mutate_archive(candidate / "gateway-factory-defaults.tar.gz", "config/factory/runtime/device_identity.json",
                            lambda data: data + b"\n ")
        self.assert_preflight_rejected(candidate, "bundle", "bundle hash mismatch: config/factory/runtime/device_identity.json")

    def test_generic_agc_source_and_hash_tampering_rejected(self):
        for variant in ("source", "hash"):
            candidate = self.copy_candidate("agc-" + variant)
            if variant == "source":
                def change_source(data):
                    manifest = json.loads(data)
                    manifest["sourceCommit"] = "0" * 40
                    return json.dumps(manifest).encode()
                member, transform = "agc-avc-runtime-manifest.json", change_source
                error = "AGC overlay source does not match generic programs"
            else:
                member, transform = "build-aarch64/AgcAvcController", lambda data: data + b"tampered"
                error = "bundle hash mismatch: build-aarch64/AgcAvcController"
            self.mutate_archive(candidate / "gateway-agc-avc-runtime.tar.gz", member, transform)
            self.assert_preflight_rejected(candidate, "agc-" + variant, error, mode="agc_avc")

    def test_generic_missing_machine_identity_rejected(self):
        candidate = self.copy_candidate("missing-machine")
        for name, machine in (("omitted", None), ("empty", ""), ("placeholder", "GW_FACTORY_001")):
            self.assert_preflight_rejected(candidate, "machine-" + name,
                                          "explicit non-placeholder machine code", machine=machine)

    def test_generic_profile_runtime_qt_matrix(self):
        records = []
        for profile in ("base", "project", "full"):
            for mode in ("gateway", "ems", "agc_avc"):
                for qt in ("off", "on"):
                    name = profile + "-" + mode + "-" + qt
                    with self.subTest(case=name):
                        home, result = self.init(name, profile, mode, qt)
                        rejected = profile == "base" and (mode == "ems" or qt == "on")
                        self.assertEqual(2 if rejected else 0, result.returncode, result.stdout)
                        if rejected:
                            self.assertFalse((home / "config/runtime").exists())
                            records.append({"case": name, "result": "EXPECTED_REJECTION"})
                            continue
                        identity = json.loads((home / "config/runtime/device_identity.json").read_text())
                        self.assertEqual("GENERIC_TEST_001", identity["machineCode"])
                        monitor = json.loads((home / "config/runtime/apps/monitor-service.json").read_text())
                        self.assertEqual(qt == "on", monitor["localDisplay"]["enabled"])
                        self.assertEqual([], monitor["deviceConfigFiles"])
                        self.assertFalse(monitor["ota"]["enabled"])
                        self.assertFalse(monitor["systemMonitor"]["cellular"]["routeFailover"]["enabled"])
                        units = subprocess.check_output(["sh", str(home / "bin/gateway-services.sh"), "list"],
                                                        env={**self.env, "GATEWAY_HOME": str(home)}, text=True)
                        self.assertEqual(qt == "on", "ky-ems.service" in units)
                        self.assertEqual(mode == "agc_avc", "agc-avc@agc-avc-service.service" in units)
                        self.assertNotIn("modbus-rtu@", units)
                        if qt == "on":
                            self.assertTrue((home / "bin/QtDisplayBridge").is_file())
                            project = home / "scada/current"
                            self.assertTrue(project.is_symlink())
                            self.assertEqual([], json.loads((project / "tags.json").read_text()))
                            nodes = json.loads((project / "nodes.json").read_text())
                            self.assertEqual("GENERIC_TEST_001", nodes[0]["machineCode"])
                            screen = json.loads((project / "screens/Points1.json").read_text())
                            self.assert_background_readable(screen, lambda name: (project / name).read_bytes())
                        if mode == "agc_avc":
                            agc = json.loads((home / "config/runtime/apps/agc-avc-service.json").read_text())["agcAvc"]
                            self.assertTrue(agc["shadowMode"])
                            self.assertFalse(agc["submitWrites"])
                        records.append({"case": name, "result": "PASS", "units": units.splitlines()})
                        self.assert_isolated()
        harness.write(harness.EVIDENCE / "profile-runtime-qt-matrix.json", json.dumps(records, indent=2))

    def test_generic_project_auto_and_full_default_no_qt(self):
        home, result = self.init("project-auto", "project", qt="auto", selected=["KY-EMS"])
        self.assertEqual(0, result.returncode, result.stdout)
        self.assertTrue((home / "scada/current").is_symlink())
        self.assertTrue((home / "bin/QtDisplayBridge").is_file())
        home, result = self.init("full-auto", qt="auto")
        self.assertEqual(0, result.returncode, result.stdout)
        self.assertFalse((home / "scada/current").exists())
        self.assert_isolated()

    def test_generic_missing_overlay_and_unknown_driver(self):
        home, result = self.init("no-overlay", mode="agc_avc", overlay=False)
        self.assertEqual(2, result.returncode, result.stdout)
        self.assertFalse((home / "bin").exists())
        home, result = self.init("unknown-driver", "project", selected=["../../outside"])
        self.assertEqual(2, result.returncode, result.stdout)
        self.assertIn("unknown or unavailable", result.stdout)
        self.assertFalse((home / "bin").exists())
        self.assert_isolated()

    def test_generic_program_bytes_and_credentials(self):
        provenance = json.loads((CANDIDATE / "component-provenance.json").read_text())
        for filename, metadata in provenance["artifacts"].items():
            self.assertEqual(metadata["sha256"], hashlib.sha256((CANDIDATE / filename).read_bytes()).hexdigest())
        binaries = {}
        def scan(data):
            if isinstance(data, dict):
                for key, value in data.items():
                    if key.lower() in ("password", "secretkey", "accesskey", "token", "apikey", "username", "imei", "serialnumber"):
                        self.assertFalse(value, "nonempty credential/identity field: " + key)
                    scan(value)
            elif isinstance(data, list):
                for value in data:
                    scan(value)
        for filename in provenance["artifacts"]:
            with tarfile.open(CANDIDATE / filename) as package:
                for member in package.getmembers():
                    self.assertFalse(member.issym() or member.islnk())
                    if not member.isfile():
                        continue
                    data = package.extractfile(member).read()
                    if data.startswith(b"\x7fELF"):
                        binaries[Path(member.name).name] = hashlib.sha256(data).hexdigest()
                    if "/config/" in member.name and member.name.endswith(".json"):
                        self.assertNotRegex(data.decode(), r"COMM\d{6,}|MTR\d{6,}|BEGIN .*PRIVATE KEY")
                        scan(json.loads(data))
        self.assertEqual(19, len(binaries))
        for component in provenance["components"]:
            self.assertEqual(component["sha256"], binaries[component["binary"]])

    def assert_background_readable(self, screen, read_asset):
        name = screen["background"]
        self.assertTrue(name.startswith("assets/") and name.endswith(".png"),
                        "Qt screen.background is an image path, not a color: " + name)
        data = read_asset(name)
        self.assertEqual(b"\x89PNG\r\n\x1a\n", data[:8])
        offset, chunks = 8, []
        while offset < len(data):
            length = struct.unpack(">I", data[offset:offset + 4])[0]
            kind = data[offset + 4:offset + 8]
            payload = data[offset + 8:offset + 8 + length]
            checksum = struct.unpack(">I", data[offset + 8 + length:offset + 12 + length])[0]
            self.assertEqual(checksum, zlib.crc32(kind + payload) & 0xffffffff)
            chunks.append((kind, payload))
            offset += length + 12
        self.assertEqual(len(data), offset)
        self.assertEqual([b"IHDR", b"IDAT", b"IEND"], [kind for kind, _ in chunks])
        self.assertEqual((1, 1, 8, 2, 0, 0, 0), struct.unpack(">IIBBBBB", chunks[0][1]))
        self.assertEqual(b"", chunks[-1][1])
        pixel = zlib.decompress(chunks[1][1])
        self.assertEqual(bytes((0, 244, 246, 248)), pixel)
        def luminance(rgb):
            channels = [channel / 255.0 for channel in rgb]
            linear = [channel / 12.92 if channel <= 0.04045 else ((channel + 0.055) / 1.055) ** 2.4
                      for channel in channels]
            return sum(channel * weight for channel, weight in zip(linear, (0.2126, 0.7152, 0.0722)))
        background = luminance(pixel[1:])
        for widget in screen["widgets"]:
            color = widget["properties"]["qtTextColor"]
            self.assertRegex(color, r"^#[0-9A-Fa-f]{6}$")
            foreground = luminance(bytes.fromhex(color[1:]))
            contrast = (max(background, foreground) + 0.05) / (min(background, foreground) + 0.05)
            self.assertGreaterEqual(contrast, 7.0, widget["widgetId"])

    def test_generic_background_asset_and_contrast(self):
        home, result = self.init("background", qt="on")
        self.assertEqual(0, result.returncode, result.stdout)
        project = home / "scada/current"
        screen = json.loads((project / "screens/Points1.json").read_text())
        self.assert_background_readable(screen, lambda name: (project / name).read_bytes())
        checksums = json.loads((project / "checksums.json").read_text())
        self.assertIn(screen["background"], checksums)
        self.assertEqual(checksums[screen["background"]], hashlib.sha256((project / screen["background"]).read_bytes()).hexdigest())
        self.assert_isolated()

    def test_generic_identity_mapping_and_duplicate_rejection(self):
        candidate = self.copy_candidate("mapping")
        home, result = self.init("mapping", qt="on", candidate=candidate)
        self.assertEqual(0, result.returncode, result.stdout)
        spec = importlib.util.spec_from_file_location("generic_runtime", candidate / "deploy/prepare-generic-runtime.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        app_path = home / "config/runtime/apps/monitor-service.json"
        app = json.loads(app_path.read_text())
        device_path = home / "config/runtime/devices/test.json"
        app["deviceConfigFiles"] = [str(device_path)]
        module.write(app_path, app)
        device = {"enabled": True, "memoryStore": {"sharedMemoryName": "gateway_fixture"}, "meters": [
            {"meterCode": "METER_A", "points": [{"index": 31, "pointCode": "V", "name": "Voltage", "unit": "V"}]},
            {"meterCode": "METER_B", "points": [{"index": 32, "pointCode": "P", "name": "Power", "unit": "kW"}]}]}
        device["meters"][1]["points"].extend(
            {"index": 33 + i, "pointCode": "P" + str(i), "name": "Extra " + str(i), "unit": "kW"}
            for i in range(16))
        module.write(device_path, device)
        package = self.root / "runtime.kyscada"
        module.make_project(home, "REMAPPED_002", package)
        with zipfile.ZipFile(package) as project:
            nodes = json.loads(project.read("nodes.json"))
            tags = json.loads(project.read("tags.json"))
            routes = json.loads(project.read("runtime-map.json"))
            expected = {meter["meterCode"] + "." + point["pointCode"]: (meter["meterCode"], point)
                        for meter in device["meters"] for point in meter["points"]}
            self.assertEqual(18, len(expected))
            self.assertEqual(1, len(nodes))
            self.assertEqual("REMAPPED_002", nodes[0]["machineCode"])
            self.assertEqual(len(expected), len(tags))
            self.assertEqual(len(tags), len(routes))
            self.assertEqual(set(expected), {tag["tagId"] for tag in tags})
            self.assertEqual(set(expected), {route["tagId"] for route in routes})
            route_by_tag = {route["tagId"]: route for route in routes}
            for tag in tags:
                meter, point = expected[tag["tagId"]]
                route = route_by_tag[tag["tagId"]]
                self.assertEqual(nodes[0]["nodeId"], tag["nodeId"])
                self.assertEqual(tag["nodeId"], route["nodeId"])
                self.assertEqual((meter, meter, point["pointCode"]), (tag["meterCode"], tag["deviceId"], tag["pointCode"]))
                self.assertEqual(point["index"], tag["indexFallback"])
                self.assertEqual(point["index"], route["index"])
                self.assertEqual("gateway_fixture", route["sharedMemoryName"])
                self.assertEqual("read", tag["access"])
                self.assertIs(False, route["writable"])
            checksums = json.loads(project.read("checksums.json"))
            self.assertEqual(len(project.namelist()), len(set(project.namelist())))
            self.assertEqual(set(project.namelist()) - {"checksums.json"}, set(checksums))
            for name, digest in checksums.items():
                self.assertEqual(digest, hashlib.sha256(project.read(name)).hexdigest())
            screens = {name: json.loads(project.read(name)) for name in checksums if name.startswith("screens/")}
            self.assertEqual({"screens/Points1.json", "screens/Points2.json"}, set(screens))
            screen_ids = {screen["screenId"] for screen in screens.values()}
            self.assertIn(json.loads(project.read("manifest.json"))["entryScreen"], screen_ids)
            bound_tags = []
            for screen in screens.values():
                self.assert_background_readable(screen, project.read)
                widgets = screen["widgets"]
                self.assertEqual(len(widgets), len({widget["widgetId"] for widget in widgets}))
                for widget in widgets:
                    geometry = widget["geometry"]
                    self.assertGreater(geometry["width"], 0)
                    self.assertGreater(geometry["height"], 0)
                    self.assertGreaterEqual(geometry["x"], 0)
                    self.assertGreaterEqual(geometry["y"], 0)
                    self.assertLessEqual(geometry["x"] + geometry["width"], screen["width"])
                    self.assertLessEqual(geometry["y"] + geometry["height"], screen["height"])
                    action = widget.get("action")
                    if action:
                        self.assertEqual({"type", "targetScreen"}, set(action))
                        self.assertEqual("navigate", action["type"])
                        self.assertIn(action["targetScreen"], screen_ids)
                    for binding in widget.get("bindings", []):
                        self.assertEqual({"nodeId", "tagId", "slot"}, set(binding))
                        self.assertEqual(nodes[0]["nodeId"], binding["nodeId"])
                        self.assertIn(binding["tagId"], expected)
                        self.assertEqual("value", binding["slot"])
                        bound_tags.append(binding["tagId"])
            self.assertCountEqual(expected, bound_tags)
        device["meters"][1]["points"][0]["index"] = 31
        module.write(device_path, device)
        with self.assertRaisesRegex(ValueError, "ambiguous"):
            module.make_project(home, "REMAPPED_002", self.root / "duplicate.kyscada")
        self.assert_isolated()

    def test_target_python36_syntax_and_command_guard(self):
        script = CANDIDATE / "deploy/prepare-generic-runtime.py"
        ast.parse(script.read_text(), feature_version=(3, 6))
        source = script.read_text()
        self.assertNotIn('add_subparsers(dest="command", required=True)', source)
        result = subprocess.run([sys.executable, str(script)], capture_output=True, text=True)
        self.assertEqual(2, result.returncode)
        self.assertIn("a command is required", result.stderr)

    def test_generic_qt_launcher_dependencies(self):
        home = self.root / "qt"
        harness.write(home / "ky-ems/KY-EMS", '#!/bin/sh\nprintf "QT_EXECUTED\\n"\n')
        harness.write(home / "bin/QtDisplayBridge", '#!/bin/sh\nexit 0\n')
        (home / "ky-ems/KY-EMS").chmod(0o755)
        (home / "bin/QtDisplayBridge").chmod(0o755)
        mock = self.root / "mock/ldd"
        harness.write(mock, '#!/bin/sh\nprintf "%s\\n" "${MOCK_DEPS:-libQt5Core.so.5 => /lib/mock}"\n')
        mock.chmod(0o755)
        env = {"GATEWAY_HOME": str(home), "QT_QPA_PLATFORM": "offscreen"}
        result = self.run_script(str(CANDIDATE / "deploy/gateway-qt-run.sh"), env=env)
        self.assertEqual(0, result.returncode, result.stdout)
        self.assertIn("QT_EXECUTED", result.stdout)
        result = self.run_script(str(CANDIDATE / "deploy/gateway-qt-run.sh"), env={**env, "MOCK_DEPS": "libQt5Widgets.so.5 => not found"})
        self.assertEqual(2, result.returncode)
        self.assertNotIn("QT_EXECUTED", result.stdout)
        result = self.run_script(str(CANDIDATE / "deploy/gateway-qt-run.sh"), env={**env, "QT_QPA_PLATFORM": "xcb"})
        self.assertEqual(2, result.returncode)
        self.assertIn("XAUTHORITY", result.stdout)
        unit = (CANDIDATE / "deploy/ky-ems.service").read_text()
        self.assertIn("Requires=qt-display-bridge.service", unit)
        self.assertIn("EnvironmentFile=-/opt/modbus-gateway/config/runtime/qt-display.env", unit)
        self.assert_isolated()

    def test_generic_qt_start_preflight_before_overwrite(self):
        mock = self.root / "mock/ldd"
        harness.write(mock, '#!/bin/sh\necho "${MOCK_DEPS:-libQt5Core.so.5 => /lib/mock}"\n')
        mock.chmod(0o755)
        for name, env, error in (("missing-library", {"MOCK_DEPS": "libQt5Core.so.5 => not found"}, "not found"),
                                 ("missing-session", {}, "XAUTHORITY")):
            with self.subTest(case=name):
                old = self.root / name / "config/runtime/keep"
                harness.write(old, "previous install\n")
                home, result = self.init(name, qt="on", normal=True, start=True, extra_env=env)
                self.assertEqual(2, result.returncode, result.stdout)
                self.assertIn(error, result.stdout)
                self.assertEqual("previous install\n", old.read_text())
                self.assertFalse((home / "bin").exists())
                self.assert_isolated()

    def test_generic_mock_normal_startup_and_protected_qt_environment(self):
        mock = self.root / "mock/ldd"
        harness.write(mock, '#!/bin/sh\necho "libQt5Core.so.5 => /lib/mock"\n')
        mock.chmod(0o755)
        authority = self.root / "Xauthority"
        harness.write(authority, "synthetic-test-no-cookie\n")
        envfile = self.root / "qt-display.env"
        harness.write(envfile, "DISPLAY=:5\nXAUTHORITY=" + str(authority) + "\nXDG_RUNTIME_DIR=" + str(self.root) + "\n")
        envfile.chmod(0o600)
        home, result = self.init("normal-qt", qt="on", normal=True, start=True,
                                 extra_env={"INIT_QT_ENV_FILE": str(envfile)})
        self.assertEqual(0, result.returncode, result.stdout)
        calls = self.log.read_text().splitlines()
        self.assertLess(calls.index("restart gateway-services.service"), calls.index("start gateway-health-watchdog.service"))
        self.assertEqual(envfile.read_text(), (home / "config/runtime/qt-display.env").read_text())
        self.assertEqual(0o600, (home / "config/runtime/qt-display.env").stat().st_mode & 0o777)
        self.assertFalse((Path("/opt/modbus-gateway")).exists(), "CLI gateway home escaped")
        self.assertEqual(b"keep-shm\n", Path("/dev/shm/gateway_point_store_fixture").read_bytes())

    def test_generic_qt_runtime_directory_contract(self):
        text = (CANDIDATE / "deploy/ky-ems.service").read_text()
        unit = configparser.ConfigParser(interpolation=None, strict=False)
        unit.read_string(text)
        service = unit["Service"]
        self.assertEqual("root", service["User"])
        self.assertEqual("ky-ems", service.get("RuntimeDirectory"))
        self.assertEqual("0700", service.get("RuntimeDirectoryMode"))
        self.assertEqual("yes", service.get("RuntimeDirectoryPreserve"))
        self.assertIn("Environment=XDG_RUNTIME_DIR=/run/ky-ems", text.splitlines())
        self.assertEqual("-/opt/modbus-gateway/config/runtime/qt-display.env", service["EnvironmentFile"])
        self.assertEqual("qt-display-bridge.service", unit["Unit"]["Requires"])
        self.assert_isolated()

    def test_generic_watchdog_start_lifecycle(self):
        mock = self.root / "mock/systemctl"
        source = mock.read_text()
        harness.write(mock, source.replace('case "$1" in',
            'if [ "$*" = "start gateway-health-watchdog.service" ] && [ "${FAIL_WATCHDOG:-0}" = 1 ]; then exit 44; fi\ncase "$1" in'))
        for name, start, env, code in (("start", True, {}, 0), ("no-start", False, {}, 0),
                                        ("business-failure", True, {"FAIL_RESTART": "1"}, 42),
                                        ("watchdog-failure", True, {"FAIL_WATCHDOG": "1"}, 44)):
            with self.subTest(case=name):
                harness.write(self.log, "")
                _, result = self.init(name, normal=True, start=start, extra_env=env)
                self.assertEqual(code, result.returncode, result.stdout)
                calls = self.log.read_text().splitlines()
                watchdog = "start gateway-health-watchdog.service"
                business = "restart gateway-services.service"
                if start:
                    self.assertIn(business, calls)
                else:
                    self.assertNotIn(business, calls)
                if start and name != "business-failure":
                    self.assertEqual(1, calls.count(watchdog))
                    self.assertLess(calls.index(business), calls.index(watchdog))
                else:
                    self.assertNotIn(watchdog, calls)
                self.assertNotIn("restart gateway-health-watchdog.service", calls)
                self.assertFalse(Path("/opt/modbus-gateway").exists())
                self.assertEqual(b"private-opt-only\n", Path("/opt/isolation-sentinel").read_bytes())
                self.assertEqual(b"keep-shm\n", Path("/dev/shm/gateway_point_store_fixture").read_bytes())

    def test_generic_rejects_unprotected_qt_environment(self):
        envfile = self.root / "unsafe.env"
        harness.write(envfile, "DISPLAY=:0\n")
        envfile.chmod(0o666)
        home, result = self.init("unsafe-env", qt="on", extra_env={"INIT_QT_ENV_FILE": str(envfile)})
        self.assertEqual(2, result.returncode)
        self.assertIn("root-owned", result.stdout)
        self.assertFalse((home / "bin").exists())
        self.assert_isolated()

    def smoke_fixture(self, name, mode="gateway", profile="full"):
        home, result = self.init(name, mode=mode, profile=profile, qt="on")
        self.assertEqual(0, result.returncode, result.stdout)
        # Smoke executes pointctl, not other AArch64 programs, with MQTT probing off.
        harness.write(home / "bin/pointctl", '#!/bin/sh\nprintf "synthetic pointctl %s\\n" "$1"\n')
        return home

    def smoke(self, home, used=40):
        mock = self.root / "smoke-tools"
        mock.mkdir(exist_ok=True)
        for name in ("python3", "awk", "sed", "grep", "head", "dirname", "basename", "find", "timeout",
                     "rm", "cat", "date", "stat", "readlink", "openssl", "ldd", "ldconfig", "sh",
                     "sort", "cut", "tr", "wc", "ls", "sleep", "curl", "sha256sum"):
            target = shutil.which(name)
            if target and not (mock / name).exists():
                (mock / name).symlink_to(target)
        harness.write(mock / "df", '#!/bin/sh\nprintf "%s\\n" "Filesystem 1024-blocks Used Available Capacity Mounted on" "fixture 10000000 4000000 6000000 ' + str(used) + '% /"\n')
        (mock / "df").chmod(0o755)
        self.assertFalse((mock / "systemctl").exists())
        return self.run_script(str(home / "bin/production-smoke-test.sh"), env={
            "GATEWAY_HOME": str(home), "PATH": str(mock), "MQTT_CONNECT_TEST": "0"})

    def test_generic_smoke_uncommissioned_contract(self):
        for mode, profile in (("gateway", "full"), ("agc_avc", "full"), ("ems", "full"), ("gateway", "project")):
            with self.subTest(mode=mode, profile=profile):
                home = self.smoke_fixture("smoke-" + mode + "-" + profile, mode, profile)
                result = self.smoke(home)
                self.assertEqual(0, result.returncode, result.stdout)
                self.assertIn("generic-uncommissioned: valid empty deviceConfigFiles", result.stdout)
                self.assertNotIn("[FAIL]", result.stdout)
                self.assertEqual(mode == "ems", (home / "bin/EmsClusterCoordinator").exists())
                manifest = json.loads((home / "config/runtime/edge-package-manifest.json").read_text())
                self.assertEqual("generic-uncommissioned", manifest["initializationKind"])
                if mode == "ems":
                    (home / "bin/EmsClusterCoordinator").unlink()
                    missing = self.smoke(home)
                    self.assertEqual(1, missing.returncode)
                    self.assertIn("EmsClusterCoordinator missing", missing.stdout)
                self.assert_isolated()

    def test_generic_smoke_keeps_invalid_config_and_disk_failures(self):
        template = self.smoke_fixture("smoke-negative-source")
        for case in ("no-marker", "bad-json", "missing-field", "wrong-type", "missing-reference", "disk-full", "valid-reference"):
            with self.subTest(case=case):
                home = self.root / ("smoke-" + case)
                shutil.copytree(template, home, symlinks=True)
                app_path = home / "config/runtime/apps/mqtt-service.json"
                app = json.loads(app_path.read_text())
                if case == "no-marker":
                    manifest_path = home / "config/runtime/edge-package-manifest.json"
                    manifest = json.loads(manifest_path.read_text())
                    manifest.pop("initializationKind", None)
                    harness.write(manifest_path, json.dumps(manifest))
                elif case == "bad-json":
                    harness.write(app_path, "{broken")
                elif case == "missing-field":
                    del app["deviceConfigFiles"]
                elif case == "wrong-type":
                    app["deviceConfigFiles"] = "not-an-array"
                elif case in ("missing-reference", "valid-reference"):
                    app["deviceConfigFiles"] = [str(home / "config/runtime/devices/fixture.json")]
                    if case == "valid-reference":
                        harness.write(Path(app["deviceConfigFiles"][0]), '{"enabled":false,"meters":[]}')
                if case not in ("bad-json", "no-marker"):
                    harness.write(app_path, json.dumps(app, indent=2))
                result = self.smoke(home, used=87 if case == "disk-full" else 40)
                self.assertEqual(0 if case == "valid-reference" else 1, result.returncode, result.stdout)
                if case == "disk-full":
                    self.assertIn("disk used 87% > 85%", result.stdout)
                elif case == "no-marker":
                    self.assertIn("no deviceConfigFiles found in app configs", result.stdout)
                elif case == "missing-reference":
                    self.assertIn("device config referenced by mqtt-service.json missing", result.stdout)
                elif case in ("bad-json", "missing-field", "wrong-type"):
                    self.assertIn("invalid device references", result.stdout)
                self.assert_isolated()


if __name__ == "__main__":
    sys.dont_write_bytecode = True
    os.environ["PYTHONDONTWRITEBYTECODE"] = "1"
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--evidence", type=Path, required=True)
    parser.add_argument("--parent-mount-ns")
    parser.add_argument("--parent-opt-device")
    parser.add_argument("--preflight-only", action="store_true")
    parser.add_argument("--test", action="append", choices=unittest.defaultTestLoader.getTestCaseNames(GenericFactoryTest),
                        help="Run only this test method; repeat to select several methods")
    args = parser.parse_args()
    if os.geteuid() != 0:
        parser.error("root required for private mounts")
    CANDIDATE = args.candidate.resolve()
    harness.EVIDENCE = args.evidence.resolve()
    harness.REPO = Path(__file__).resolve().parents[1]
    if not args.parent_mount_ns:
        command = ["unshare", "--mount", "--ipc", "--fork", sys.executable, str(Path(__file__).resolve()),
                                  "--candidate", str(CANDIDATE), "--evidence", str(harness.EVIDENCE),
                                  "--parent-mount-ns", os.readlink("/proc/self/ns/mnt"),
                                  "--parent-opt-device", str(Path("/opt").stat().st_dev)]
        if args.preflight_only:
            command.append("--preflight-only")
        for name in args.test or []:
            command.extend(["--test", name])
        sys.exit(subprocess.call(command))
    if args.parent_mount_ns == os.readlink("/proc/self/ns/mnt"):
        parser.error("private mount namespace missing")
    subprocess.run(["mount", "--make-rprivate", "/"], check=True)
    for directory in harness.PROTECTED:
        subprocess.run(["mount", "-t", "tmpfs", "-o", "mode=755", "tmpfs", directory], check=True)
    if str(Path("/opt").stat().st_dev) == args.parent_opt_device:
        parser.error("/opt still addresses the parent filesystem")
    harness.EVIDENCE.mkdir(parents=True, exist_ok=True)
    proof = {}
    for directory in harness.PROTECTED:
        device = Path(directory).stat().st_dev
        device_id = str(os.major(device)) + ":" + str(os.minor(device))
        observed = json.loads(subprocess.check_output(
            ["findmnt", "--json", "-o", "TARGET,FSTYPE,MAJ:MIN,PROPAGATION", "--target", directory], text=True))["filesystems"]
        resolved = [item for item in observed if item["target"] == directory and item["maj:min"] == device_id]
        if len(resolved) != 1 or resolved[0]["fstype"] != "tmpfs" or resolved[0]["propagation"] != "private":
            parser.error("protected path is not a resolved private tmpfs: " + directory)
        proof[directory] = {"resolved": resolved[0], "observed": observed}
    harness.write(harness.EVIDENCE / "namespace-proof.json", json.dumps({"mounts": proof, "parentOptDevice": args.parent_opt_device,
                  "privateOptDevice": str(Path("/opt").stat().st_dev), "namespace": os.readlink("/proc/self/ns/mnt")}, indent=2))
    if args.preflight_only:
        print("PRIVATE_MOUNTS_VERIFIED: " + ", ".join(proof))
        sys.exit(0)
    def candidate_snapshot():
        return {str(path.relative_to(CANDIDATE)): hashlib.sha256(path.read_bytes()).hexdigest()
                for path in CANDIDATE.rglob("*") if path.is_file()}
    before = candidate_snapshot()
    suite = (unittest.TestSuite(GenericFactoryTest(name) for name in args.test) if args.test else
             unittest.defaultTestLoader.loadTestsFromTestCase(GenericFactoryTest))
    with (harness.EVIDENCE / "result.txt").open("w") as output:
        result = unittest.TextTestRunner(stream=output, verbosity=2).run(suite)
    after = candidate_snapshot()
    harness.write(harness.EVIDENCE / "candidate-integrity.json", json.dumps(
        {"unchanged": before == after, "before": before, "after": after,
         "selectedTests": args.test, "bytecodeDisabled": sys.dont_write_bytecode,
         "testSourceSha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest()}, indent=2))
    print((harness.EVIDENCE / "result.txt").read_text())
    sys.exit(0 if result.wasSuccessful() and before == after else 1)
