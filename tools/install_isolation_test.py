#!/usr/bin/env python3
"""Offline installer regression: root + private Linux mount/IPC namespaces only."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest
import zipfile


def write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


class InstallIsolationTest(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp(prefix="gateway-install-test-", dir="/var/tmp"))
        self.addCleanup(shutil.rmtree, self.root)
        # These directories are private tmpfs mounts, never the host directories.
        for directory in PROTECTED:
            for entry in Path(directory).iterdir():
                if entry.is_dir() and not entry.is_symlink():
                    shutil.rmtree(entry)
                else:
                    entry.unlink()
        self.marker = Path("/run/gateway-health-watchdog/applying")
        write(self.marker, "kind=other-install\npid=999999\n")
        write(Path("/run/gateway-health-watchdog/manual-stop"), "keep\n")
        write(Path("/dev/shm/gateway_point_store_fixture"), "keep-shm\n")
        write(Path("/tmp/unrelated"), "keep-tmp\n")
        self.log = self.root / "systemctl.log"
        write(self.log, "")
        mock = self.root / "mock"
        write(mock / "systemctl", """#!/bin/sh
printf '%s\\n' "$*" >> "$SYSTEMCTL_LOG"
case "$1" in
  list-unit-files) echo 'ky-ems.service enabled' ;;
  show) echo 4242 ;;
  restart) [ "${FAIL_RESTART:-0}" != 1 ] || exit 42 ;;
esac
exit 0
""")
        write(mock / "sleep", "#!/bin/sh\nexit 0\n")
        write(mock / "cp", """#!/bin/sh
for arg do
  case "$arg" in
    */bin/MqttDriver) [ "${FAIL_COPY:-0}" != 1 ] || exit 43 ;;
  esac
done
exec /bin/cp "$@"
""")
        for path in mock.iterdir():
            path.chmod(0o755)
        self.env = {"PATH": f"{mock}:/usr/sbin:/usr/bin:/sbin:/bin",
                    "HOME": str(self.root), "LANG": "C", "SYSTEMCTL_LOG": str(self.log)}
        self.before = self.snapshot()

    def snapshot(self):
        result = {}
        for directory in PROTECTED:
            for path in Path(directory).rglob("*"):
                result[str(path)] = ("link", os.readlink(path)) if path.is_symlink() else (
                    ("dir",) if path.is_dir() else ("file", path.read_bytes()))
        return result

    def run_script(self, script, args=(), env=None):
        process = subprocess.run(["sh", str(REPO / "deploy" / script), *map(str, args)],
                                 env={**self.env, **(env or {})}, stdin=subprocess.DEVNULL,
                                 text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                 timeout=30)
        with (EVIDENCE / (self.id().split(".")[-1] + ".log")).open("a") as output:
            output.write(f"script={script} args={args} exit={process.returncode}\n{process.stdout}\n")
        return process

    def factory(self):
        source = self.root / "source"
        shutil.copytree(REPO / "deploy", source / "deploy")
        for binary in ("SystemMonitor", "MqttDriver", "MqttForwarder", "pointctl"):
            write(source / "build-aarch64" / binary, "fixture-not-executable\n")
        for app in ("monitor-service", "mqtt-service"):
            write(source / "config/factory/runtime/apps" / (app + ".json"),
                  '{"runtimeMode":"gateway","deviceConfigFiles":[]}\n')
        return {"GATEWAY_HOME": str(self.root / "gateway"), "SOURCE_ROOT": str(source),
                "DEFAULT_SOURCE_ROOT": str(source), "DEPLOY_DIR": str(source / "deploy"),
                "FACTORY_DIR": str(source / "config/factory"), "TEMPLATES_DIR": str(source / "none"),
                "EXAMPLES_DIR": str(source / "none"), "INSTALL_SYSTEMD": "0", "START_SERVICES": "0",
                "RESET_SHM": "0", "INIT_PROMPT": "0", "INIT_MQTT_TLS_ENABLED": "false",
                "INIT_DIRECT_MAINTENANCE_ENABLED": "0", "PACKAGE_PROFILE": "base"}

    def assert_isolated(self):
        self.assertEqual(self.before, self.snapshot(), "protected filesystem changed")
        self.assertEqual("", self.log.read_text(), "isolated mode invoked systemctl")

    def test_factory_isolated_success(self):
        env = self.factory()
        result = self.run_script("install-factory-config.sh", env=env)
        self.assertEqual(0, result.returncode, result.stdout)
        self.assertTrue((self.root / "gateway/bin/SystemMonitor").is_file())
        self.assert_isolated()

    def test_factory_isolated_preflight_failure(self):
        env = self.factory()
        (self.root / "source/build-aarch64/SystemMonitor").unlink()
        result = self.run_script("install-factory-config.sh", env=env)
        self.assertNotEqual(0, result.returncode)
        self.assertIn("required binary missing", result.stdout)
        self.assert_isolated()

    def test_factory_isolated_copy_failure(self):
        result = self.run_script("install-factory-config.sh", env={**self.factory(), "FAIL_COPY": "1"})
        self.assertNotEqual(0, result.returncode)
        self.assert_isolated()

    def test_factory_normal_success(self):
        result = self.run_script("install-factory-config.sh", env={**self.factory(), "INSTALL_SYSTEMD": "1"})
        self.assertEqual(0, result.returncode, result.stdout)
        self.assertTrue(Path("/etc/default/gateway-network-failover").is_file())
        self.assertTrue(Path("/etc/systemd/system/gateway-services.service").is_file())
        self.assertIn("stop gateway-services.service", self.log.read_text())
        self.assertIn("daemon-reload", self.log.read_text())
        self.assertFalse(self.marker.exists(), "own applying marker must be removed")
        self.assertEqual(b"keep-shm\n", Path("/dev/shm/gateway_point_store_fixture").read_bytes())

    def test_factory_normal_copy_failure(self):
        result = self.run_script("install-factory-config.sh", env={**self.factory(), "INSTALL_SYSTEMD": "1", "FAIL_COPY": "1"})
        self.assertNotEqual(0, result.returncode)
        self.assertFalse(self.marker.exists(), "own applying marker leaked on failure")
        self.assertNotIn("start gateway-services.service", self.log.read_text())

    def test_factory_rejects_isolated_side_effect_flags(self):
        env = self.factory()
        for flag in ("START_SERVICES", "RESET_SHM"):
            result = self.run_script("install-factory-config.sh", env={**env, flag: "1"})
            self.assertEqual(2, result.returncode, result.stdout)
        self.assert_isolated()

    def scada(self):
        documents = {"manifest.json": {"schemaVersion": "2.0", "projectId": "fixture", "packageVersion": "1"},
                     "topology.json": {"mode": "integrated"},
                     "nodes.json": [{"nodeId": "edge", "machineCode": "TEST"}],
                     "tags.json": [], "runtime-map.json": []}
        contents = {key: json.dumps(value).encode() for key, value in documents.items()}
        contents["checksums.json"] = json.dumps({key: hashlib.sha256(value).hexdigest()
                                                 for key, value in contents.items()}).encode()
        package = self.root / "fixture.kyscada"
        with zipfile.ZipFile(package, "w") as archive:
            for key, value in contents.items():
                archive.writestr(key, value)
        self.app = self.root / "monitor.json"
        write(self.app, '{"marker":"old","localDisplay":{"enabled":true}}\n')
        self.app_before = self.app.read_bytes()
        self.scada_root = self.root / "scada"
        write(self.scada_root / "releases/old/keep", "old-release\n")
        (self.scada_root / "current").symlink_to("releases/old")
        return ["--package", package, "--machine-code", "TEST", "--app-config", self.app,
                "--scada-root", self.scada_root]

    def assert_scada_clean(self):
        self.assertEqual([], list(self.root.rglob("gateway-scada-meta.*")), "metadata leaked")
        self.assertEqual([], list(self.scada_root.glob("current.*")), "temporary activation link leaked")
        self.assertEqual([], list(self.scada_root.rglob("*.staging")), "staging leaked")

    def test_scada_default_restart(self):
        result = self.run_script("install-scada-project.sh", [*self.scada(), "--restart"])
        self.assertEqual(0, result.returncode, result.stdout)
        self.assertIn("restart ky-ems.service", self.log.read_text())
        self.assertIn("show ky-ems.service -p MainPID --value", self.log.read_text())
        self.assert_scada_clean()
        self.assertEqual(self.before, self.snapshot())

    def test_scada_isolated_restart_rejected(self):
        result = self.run_script("install-scada-project.sh", [*self.scada(), "--restart"], {"INSTALL_SYSTEMD": "0"})
        self.assertEqual(2, result.returncode, result.stdout)
        self.assertEqual(self.app_before, self.app.read_bytes())
        self.assertEqual("releases/old", os.readlink(self.scada_root / "current"))
        self.assert_scada_clean()
        self.assert_isolated()

    def test_scada_isolated_success(self):
        result = self.run_script("install-scada-project.sh", self.scada(), {"INSTALL_SYSTEMD": "0"})
        self.assertEqual(0, result.returncode, result.stdout)
        self.assertNotEqual("releases/old", os.readlink(self.scada_root / "current"))
        self.assert_scada_clean()
        self.assert_isolated()

    def test_scada_isolated_rollback(self):
        args = self.scada()
        write(self.root / "not-a-directory", "block state-file write\n")
        result = self.run_script("install-scada-project.sh", [*args, "--state-file", self.root / "not-a-directory/state"],
                                 {"INSTALL_SYSTEMD": "0"})
        self.assertNotEqual(0, result.returncode)
        self.assertIn("rolling back", result.stdout)
        self.assertEqual(self.app_before, self.app.read_bytes())
        self.assertEqual("releases/old", os.readlink(self.scada_root / "current"))
        self.assertEqual(["old"], sorted(p.name for p in (self.scada_root / "releases").iterdir()))
        self.assert_scada_clean()
        self.assert_isolated()

    def test_scada_restart_failure_rollback(self):
        result = self.run_script("install-scada-project.sh", [*self.scada(), "--restart"], {"FAIL_RESTART": "1"})
        self.assertNotEqual(0, result.returncode)
        self.assertIn("rolling back", result.stdout)
        self.assertEqual(2, self.log.read_text().count("restart ky-ems.service"))
        self.assertEqual(self.app_before, self.app.read_bytes())
        self.assertEqual("releases/old", os.readlink(self.scada_root / "current"))
        self.assert_scada_clean()

    def test_init_preserves_preexisting_work_directory(self):
        work = self.root / "existing-work"
        write(work / "keep", "another invocation\n")
        result = self.run_script("production-init.sh", ["--auto", "--package", self.root / "missing.tar.gz",
                                 "--no-start", "--no-smoke"],
                                 {"INSTALL_SYSTEMD": "0", "INIT_WORK_DIR": str(work),
                                  "GATEWAY_HOME": str(self.root / "gateway")})
        self.assertNotEqual(0, result.returncode)
        self.assertTrue((work / "keep").is_file(), "removed unowned work directory")
        self.assert_isolated()

    def test_init_rejects_isolated_side_effect_flags(self):
        for flags in (["--start", "--no-smoke"], ["--no-start", "--smoke"],
                      ["--no-start", "--no-smoke", "--reset-shm"]):
            result = self.run_script("production-init.sh", ["--auto", *flags],
                                     {"INSTALL_SYSTEMD": "0", "GATEWAY_HOME": str(self.root / "gateway")})
            self.assertEqual(2, result.returncode, result.stdout)
        self.assert_isolated()


PROTECTED = ("/etc/default", "/etc/systemd", "/run", "/tmp", "/dev/shm")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--evidence", required=True, type=Path)
    parser.add_argument("--parent-mount-ns")
    options = parser.parse_args()
    if os.geteuid() != 0:
        parser.error("requires root for private mounts; never run installers outside the namespace")
    EVIDENCE = options.evidence.resolve()
    REPO = Path(__file__).resolve().parent.parent
    if not options.parent_mount_ns:
        sys.exit(subprocess.call(["unshare", "--mount", "--ipc", "--fork", sys.executable, str(Path(__file__).resolve()),
                                  "--evidence", str(EVIDENCE), "--parent-mount-ns", os.readlink("/proc/self/ns/mnt")]))
    if options.parent_mount_ns == os.readlink("/proc/self/ns/mnt"):
        parser.error("private mount namespace was not created")
    subprocess.run(["mount", "--make-rprivate", "/"], check=True)
    for directory in PROTECTED:
        subprocess.run(["mount", "-t", "tmpfs", "-o", "mode=755", "tmpfs", directory], check=True)
    EVIDENCE.mkdir(parents=True, exist_ok=True)
    with (EVIDENCE / "result.txt").open("w") as output:
        result = unittest.TextTestRunner(stream=output, verbosity=2).run(
            unittest.defaultTestLoader.loadTestsFromTestCase(InstallIsolationTest))
    print((EVIDENCE / "result.txt").read_text())
    sys.exit(0 if result.wasSuccessful() else 1)
