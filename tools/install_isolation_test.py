#!/usr/bin/env python3
"""Offline installer regression: root + private Linux mount/IPC namespaces only."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import struct
import sys
import tarfile
import tempfile
import time
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
[ "$1" != "${FAIL_SYSTEMCTL_ACTION:-none}" ] || exit 42
case "$1" in
  list-unit-files) echo 'ky-ems.service enabled' ;;
  show)
    if [ "${OFFLINE_TEST:-0}" = 1 ]; then
      case "$*" in
        *UnitFileState*) echo disabled ;;
        *) echo inactive ;;
      esac
    else echo 4242; fi ;;
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

    def test_ota_rollback_service_failure_is_not_success(self):
        job = "rollback-failure"
        backup = self.root / "backup"
        staging = self.root / "staging"
        write(backup / job / "previous_version.txt", "old-version\n")
        write(staging / "applied_version.txt", "candidate-version\n")
        write(staging / job / "restart_services.txt", "compute-engine@test.service\n")
        result = self.run_script("ota-rollback.sh",
                                 [self.root / "artifact.tar.gz", "candidate", job, backup, staging],
                                 {"FAIL_SYSTEMCTL_ACTION": "restart"})
        self.assertNotEqual(0, result.returncode, result.stdout)
        self.assertNotIn("success jobId=", result.stdout)
        self.assertEqual("candidate-version\n", (staging / "applied_version.txt").read_text())
        self.assertTrue(Path("/run/gateway-health-watchdog/manual-stop").is_file())

    def test_ota_rollback_service_boundaries(self):
        for action in ("daemon-reload", "enable", "reload-or-restart", "is-active"):
            with self.subTest(action=action):
                job = "rollback-" + action
                backup = self.root / "backup"
                staging = self.root / "staging"
                write(backup / job / "previous_version.txt", "old\n")
                write(staging / "applied_version.txt", "candidate\n")
                write(staging / job / "restart_services.txt", "gateway-services.service\n")
                result = self.run_script("ota-rollback.sh",
                                         [self.root / "artifact.tar.gz", "candidate", job, backup, staging],
                                         {"FAIL_SYSTEMCTL_ACTION": action})
                self.assertNotEqual(0, result.returncode, result.stdout)
                self.assertNotIn("success jobId=", result.stdout)
                self.assertIn("status=failed", (staging / ("rollback_" + job + ".txt")).read_text())
                self.assertEqual("candidate\n", (staging / "applied_version.txt").read_text())

    def test_ota_rollback_preserves_identity_and_durable_data(self):
        job = "rollback-preserve"
        backup = self.root / "backup"
        staging = self.root / "staging"
        live = Path("/opt/modbus-gateway")
        preserved = {"data/control-dedup.sqlite": "new durable dedup state\n",
                     "data/cluster-membership.json": '{"membershipEpoch":2}\n',
                     "data/ems-cluster-consensus.json": '{"term":17}\n',
                     "config/runtime/device_identity.json": '{"machineCode":"LOCAL"}\n'}
        for name, value in preserved.items():
            write(live / name, value)
            write(backup / job / "opt/modbus-gateway" / name, "old snapshot must not replace live state\n")
        write(backup / job / "opt/modbus-gateway/config/runtime/apps/monitor.json", '{"old":true}\n')
        write(backup / job / "previous_version.txt", "old-version\n")
        write(staging / job / "restart_services.txt", "compute-engine@test.service\n")
        result = self.run_script("ota-rollback.sh",
                                 [self.root / "artifact.tar.gz", "candidate", job, backup, staging])
        self.assertEqual(0, result.returncode, result.stdout)
        for name, value in preserved.items():
            self.assertEqual(value, (live / name).read_text(), name)
        self.assertEqual('{"old":true}\n', (live / "config/runtime/apps/monitor.json").read_text())
        self.assertEqual("old-version\n", (staging / "applied_version.txt").read_text())

    def test_scada_rollback_failure_does_not_stop_gateway(self):
        staging = self.root / "staging"
        job = "scada-failure"
        write(staging / job / "package-type.txt", "packageType=scada\n")
        write(staging / job / "scada-install-state.txt", "scadaRoot=/invalid\n")
        before = Path("/run/gateway-health-watchdog/manual-stop").read_bytes()
        result = self.run_script("ota-rollback.sh",
                                 [self.root / "artifact.kyscada", "candidate", job, self.root / "backup", staging])
        self.assertNotEqual(0, result.returncode)
        self.assertEqual(before, Path("/run/gateway-health-watchdog/manual-stop").read_bytes())
        self.assertEqual("", self.log.read_text())

    def test_ota_rollback_marker_failure_still_cleans_up(self):
        staging = self.root / "staging"
        job = "marker-failure"
        write(staging / job / "restart_services.txt", "compute-engine@test.service\n")
        manual = Path("/run/gateway-health-watchdog/manual-stop")
        manual.unlink()
        manual.mkdir()
        result = self.run_script("ota-rollback.sh",
                                 [self.root / "artifact.tar.gz", "candidate", job, self.root / "backup", staging],
                                 {"FAIL_SYSTEMCTL_ACTION": "restart"})
        self.assertEqual(42, result.returncode, result.stdout)
        self.assertFalse(self.marker.exists())
        self.assertIn("stop compute-engine@test.service", self.log.read_text())

    def test_ota_rollback_tls_implicit_unit_is_stopped_on_failure(self):
        job = "tls-failure"
        backup = self.root / "backup"
        staging = self.root / "staging"
        write(backup / job / "opt/modbus-gateway/config/runtime/tls/fixture", "test-only\n")
        write(staging / job / "restart_services.txt", "compute-engine@test.service\n")
        result = self.run_script("ota-rollback.sh",
                                 [self.root / "artifact.tar.gz", "candidate", job, backup, staging],
                                 {"FAIL_SYSTEMCTL_ACTION": "reload-or-restart"})
        self.assertNotEqual(0, result.returncode)
        self.assertIn("stop gateway-services.service", self.log.read_text())
        self.assertNotIn("stop unrelated", self.log.read_text())

    def ota_package(self, target, contents, compatibility=None):
        package = self.root / "ota-package"
        write(package / "payload", contents)
        manifest = {"version": "fixture", "files": [{"path": "payload", "target": target,
                    "sha256": hashlib.sha256(contents.encode()).hexdigest()}], "restart": {"services": []}}
        if compatibility is not None:
            manifest["runtimeCompatibility"] = compatibility
        write(package / "manifest.json", json.dumps(manifest))
        artifact = self.root / "fixture.tar.gz"
        with tarfile.open(artifact, "w:gz") as archive:
            archive.add(package / "manifest.json", arcname="manifest.json")
            archive.add(package / "payload", arcname="payload")
        return [artifact, "fixture", "fixture-job", self.root / "backup", self.root / "staging"]

    def test_ota_runtime_rejected_before_any_live_change(self):
        target = Path("/opt/modbus-gateway/bin/ModbusRtu")
        write(target, "old runtime\n")
        args = self.ota_package(str(target), "candidate runtime\n",
                                {"pointStoreAbi": 11, "clusterProtocol": 2, "upgradeMode": "offline-all-participants"})
        result = self.run_script("ota-apply.sh", args)
        self.assertNotEqual(0, result.returncode, result.stdout)
        self.assertIn("offline all-participant", result.stdout)
        self.assertEqual("old runtime\n", target.read_text())
        self.assertEqual("", self.log.read_text())

    def test_ota_plain_config_is_not_blocked(self):
        target = Path("/opt/modbus-gateway/config/runtime/logic/fixture.json")
        result = self.run_script("ota-apply.sh", self.ota_package(str(target), '{"value":1}\n'))
        self.assertEqual(0, result.returncode, result.stdout)
        self.assertEqual('{"value":1}\n', target.read_text())

    def test_ota_compatibility_type_missing_and_conflict_rejected(self):
        valid = {"pointStoreAbi": 11, "clusterProtocol": 2, "upgradeMode": "offline-all-participants"}
        for bad in ({}, {**valid, "pointStoreAbi": "11"}, {**valid, "clusterProtocol": True},
                    {**valid, "pointStoreAbi": 10}, {**valid, "upgradeMode": "rolling"},
                    {**valid, "unknown": 1}):
            with self.subTest(compatibility=bad):
                target = Path("/opt/modbus-gateway/config/runtime/logic/fixture.json")
                result = self.run_script("ota-apply.sh", self.ota_package(str(target), '{}\n', bad))
                self.assertNotEqual(0, result.returncode)
                self.assertIn("runtimeCompatibility", result.stdout)
                self.assertFalse(target.exists())

    def test_runtime_rollback_refuses_unknown_abi_backup(self):
        target = Path("/opt/modbus-gateway/bin/ComputeEngine")
        write(target, "current runtime\n")
        backup = self.root / "backup"
        write(backup / "job/opt/modbus-gateway/bin/ComputeEngine", "old runtime\n")
        result = self.run_script("ota-rollback.sh",
                                 [self.root / "artifact.tar.gz", "candidate", "job", backup, self.root / "staging"])
        self.assertNotEqual(0, result.returncode)
        self.assertIn("offline all-participant recovery", result.stdout)
        self.assertEqual("current runtime\n", target.read_text())
        self.assertTrue(Path("/run/gateway-health-watchdog/manual-stop").is_file())

    def test_legacy_v9_entrypoint_retired_without_side_effects(self):
        result = self.run_script("upgrade-legacy-runtime-v9.sh")
        self.assertNotEqual(0, result.returncode)
        self.assertIn("retired", result.stdout)
        self.assert_isolated()

    def offline_fixture(self, modes=("create",)):
        fixture = EVIDENCE / "offline-upgrade-fixture"
        if not fixture.exists():
            command = ["g++", "-std=c++14", "-O0", "-pthread", str(REPO / "tools/offline_upgrade_fixture.cpp"), "-lrt", "-o", str(fixture)]
            result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=60)
            write(EVIDENCE / "fixture-compile.log", result.stdout)
            self.assertEqual(0, result.returncode, result.stdout)
        home = Path('/opt/modbus-gateway')
        write(home / 'bin/ComputeEngine', 'old compute\n')
        write(home / 'bin/MqttDriver', 'old mqtt\n')
        write(home / 'config/runtime/device_identity.json', '{"machineCode":"A"}\n')
        write(home / 'data/cluster-membership.json', '{"members":[{"nodeId":"A","cabinetNo":1},{"nodeId":"B","cabinetNo":2}]}\n')
        write(home / 'data/control-dedup.sqlite', 'current durable dedup\n')
        write(home / 'data/ems-cluster-consensus.json', '{"term":19}\n')
        segments = []
        for i, mode in enumerate(modes):
            source, target = 'offline_source_' + str(i), 'offline_target_' + str(i)
            subprocess.run([str(fixture), mode, source], check=True, stdout=subprocess.PIPE)
            segments.append({'source': source, 'target': target, 'sha256': hashlib.sha256((Path('/dev/shm') / source).read_bytes()).hexdigest()})
        write(home / 'config/runtime/apps/compute.json', json.dumps({'computeEngine': {'sharedMemoryNames': [s['source'] for s in segments], 'outputSharedMemoryName': segments[0]['source']}, 'emsCluster': {'enabled': True, 'controlEnabled': False, 'virtualSharedMemoryName': segments[0]['source']}}))
        payload = self.root / 'payload'
        products = [('ComputeEngine', Path('/usr/bin/true')), ('MqttDriver', Path('/usr/bin/false')),
                    ('memory_point_store_migrate', Path('/var/tmp/ems-production-candidate-20260924-build/memory_point_store_migrate'))]
        components = []
        for name, source in products:
            dest = payload / 'programs/bin' / name
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, dest)
            dest.chmod(0o755)
            components.append({'kind': 'product', 'target': name, 'archivePath': 'programs/bin/' + name,
                               'bytes': dest.stat().st_size, 'sha256': hashlib.sha256(dest.read_bytes()).hexdigest()})
        manifest = self.root / 'program-manifest.json'
        write(manifest, json.dumps({'sourceCommit': 'native-fixture-not-ARM-release', 'components': components}))
        config = home / 'config/runtime'
        sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
        approval = {'schemaVersion': 'offline-shm11-1', 'transactionId': 'fixture-offline',
                    'runtimeCompatibility': {'pointStoreAbi': 11, 'clusterProtocol': 2, 'upgradeMode': 'offline-all-participants'},
                    'controlEnabled': False, 'expiresAtUnix': int(time.time()) + 300,
                    'gatewayHome': str(home), 'nodeId': 'A', 'programManifestSha256': sha(manifest),
                    'identitySha256': sha(config / 'device_identity.json'), 'membershipSha256': sha(home / 'data/cluster-membership.json'),
                    'installedRuntimeSha256': {n: sha(home / 'bin' / n) for n in ('ComputeEngine', 'MqttDriver')},
                    'configSha256': {str(p.relative_to(config)): sha(p) for p in config.rglob('*') if p.is_file()},
                    'units': ['gateway-services.service', 'gateway-health-watchdog.service', 'ky-ems.service', 'compute-engine@fixture.service'],
                    'offlineVoters': {n: {'controlDisabled': True, 'participantsStopped': True, 'restartInhibited': True, 'evidenceSha256': 'a' * 64} for n in ('A', 'B')},
                    'segments': segments}
        write(self.root / 'approval.json', json.dumps(approval))
        return home, approval

    def offline_run(self, action='apply', approval=None, env=None):
        if approval is not None:
            write(self.root / 'approval.json', json.dumps(approval))
        pin = hashlib.sha256((self.root / 'approval.json').read_bytes()).hexdigest()
        command = ['python3', str(REPO / 'deploy/offline-runtime-upgrade.py'), action,
                   '--approval-sha256', pin, '--state', str(self.root / 'transaction')]
        if action == 'apply':
            command += ['--approval', str(self.root / 'approval.json'), '--manifest', str(self.root / 'program-manifest.json'), '--payload', str(self.root / 'payload')]
        result = subprocess.run(command, env={**self.env, 'OFFLINE_TEST': '1', **(env or {})},
                                stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=120)
        with (EVIDENCE / (self.id().split('.')[-1] + '.log')).open('a') as stream:
            stream.write('action=' + action + ' exit=' + str(result.returncode) + '\n' + result.stdout + '\n')
        return result

    def test_offline_upgrade_actual_cli_and_stopped_recovery(self):
        home, approval = self.offline_fixture()
        originals = {str(p): p.read_bytes() for p in (home / 'config').rglob('*') if p.is_file()}
        data = {str(p): p.read_bytes() for p in (home / 'data').iterdir()}
        result = self.offline_run()
        self.assertEqual(0, result.returncode, result.stdout)
        self.assertIn('UPGRADED_STOPPED', result.stdout)
        target = Path('/dev/shm/offline_target_0')
        target_sha = hashlib.sha256(target.read_bytes()).hexdigest()
        subprocess.run([str(EVIDENCE / 'offline-upgrade-fixture'), 'verify', 'offline_target_0'], check=True)
        state = json.loads((self.root / 'transaction/state.json').read_text())
        self.assertEqual(target_sha, state['segments'][0]['targetSha256'])
        self.assertEqual(approval['segments'][0]['sha256'], hashlib.sha256(Path('/dev/shm/offline_source_0').read_bytes()).hexdigest())
        self.assertIn('offline_target_0', (home / 'config/runtime/apps/compute.json').read_text())
        self.assertNotIn('offline_source_0', (home / 'config/runtime/apps/compute.json').read_text())
        self.assertNotEqual(b'old compute\n', (home / 'bin/ComputeEngine').read_bytes())
        self.assertNotIn('\nstart ', '\n' + self.log.read_text())
        self.assertNotIn('unmask', self.log.read_text())
        self.assertNotIn('disable ', self.log.read_text())
        blocked = self.run_script('gateway-services.sh', ['start'])
        self.assertNotEqual(0, blocked.returncode)
        self.assertIn('persistent offline upgrade fence', blocked.stdout)
        write(home / 'data/control-dedup.sqlite', 'newer durable state after upgrade\n')
        data[str(home / 'data/control-dedup.sqlite')] = b'newer durable state after upgrade\n'
        result = self.offline_run('recover')
        self.assertEqual(0, result.returncode, result.stdout)
        self.assertIn('RECOVERED_STOPPED', result.stdout)
        self.assertEqual(b'old compute\n', (home / 'bin/ComputeEngine').read_bytes())
        for name, value in {**originals, **data}.items():
            self.assertEqual(value, Path(name).read_bytes(), name)
        self.assertEqual(target_sha, hashlib.sha256(target.read_bytes()).hexdigest())
        self.assertTrue((home / 'data/runtime-upgrade-stop').exists())

    def test_offline_upgrade_pending_owner_and_partial_migration_refused(self):
        home, approval = self.offline_fixture(('create', 'pending'))
        result = self.offline_run()
        self.assertNotEqual(0, result.returncode)
        self.assertIn('migration refused/failed', result.stdout)
        self.assertTrue(Path('/dev/shm/offline_target_0').exists())
        self.assertFalse(Path('/dev/shm/offline_target_1').exists())
        self.assertEqual('old compute\n', (home / 'bin/ComputeEngine').read_text())
        self.assertIn('offline_source_0', (home / 'config/runtime/apps/compute.json').read_text())
        for segment in approval['segments']:
            self.assertEqual(segment['sha256'], hashlib.sha256((Path('/dev/shm') / segment['source']).read_bytes()).hexdigest())
        result = self.offline_run('recover')
        self.assertEqual(0, result.returncode, result.stdout)

    def test_offline_upgrade_owner_refused(self):
        home, _ = self.offline_fixture(('owner',))
        result = self.offline_run()
        self.assertNotEqual(0, result.returncode)
        self.assertIn('migration refused/failed', result.stdout)
        self.assertTrue((home / 'data/runtime-upgrade-stop').exists())
        self.assertFalse(Path('/dev/shm/offline_target_0').exists())

    def test_offline_upgrade_live_mapping_refused(self):
        home, _ = self.offline_fixture()
        child = subprocess.Popen(['python3', '-c', "import mmap,sys,time; f=open('/dev/shm/offline_source_0','r+b'); m=mmap.mmap(f.fileno(),0); print('mapped',flush=True); sys.stdin.read()"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        try:
            self.assertEqual('mapped\n', child.stdout.readline())
            result = self.offline_run()
            self.assertNotEqual(0, result.returncode)
            self.assertIn('migration refused/failed', result.stdout)
            self.assertFalse(Path('/dev/shm/offline_target_0').exists())
        finally:
            child.communicate('', timeout=5)
        self.assertEqual('old compute\n', (home / 'bin/ComputeEngine').read_text())

    def test_offline_upgrade_external_pins_and_full_voters_required(self):
        home, approval = self.offline_fixture()
        del approval['offlineVoters']['B']
        result = self.offline_run(approval=approval)
        self.assertNotEqual(0, result.returncode)
        self.assertIn('all fixed voters', result.stdout)
        self.assertFalse((home / 'data/runtime-upgrade-stop').exists())
        write(self.root / 'payload/programs/bin/ComputeEngine', 'tampered\n')
        result = self.offline_run()
        self.assertNotEqual(0, result.returncode)
        self.assertIn('component hash/size mismatch', result.stdout)
        self.assertNotIn('\nstop ', '\n' + self.log.read_text())

    def test_offline_upgrade_partial_file_switch_recovers_stopped(self):
        home, _ = self.offline_fixture()
        target = home / 'bin/MqttDriver'
        subprocess.run(['mount', '--bind', str(target), str(target)], check=True)
        try:
            result = self.offline_run()
            self.assertNotEqual(0, result.returncode)
            self.assertNotEqual('old compute\n'.encode(), (home / 'bin/ComputeEngine').read_bytes())
            self.assertEqual('old mqtt\n', target.read_text())
        finally:
            subprocess.run(['umount', str(target)], check=True)
        result = self.offline_run('recover')
        self.assertEqual(0, result.returncode, result.stdout)
        self.assertEqual('old compute\n', (home / 'bin/ComputeEngine').read_text())
        self.assertTrue((home / 'data/runtime-upgrade-stop').exists())

    def test_offline_upgrade_stop_failure_changes_no_runtime(self):
        home, _ = self.offline_fixture()
        result = self.offline_run(env={'FAIL_SYSTEMCTL_ACTION': 'stop'})
        self.assertNotEqual(0, result.returncode)
        self.assertEqual('old compute\n', (home / 'bin/ComputeEngine').read_text())
        self.assertTrue((home / 'data/runtime-upgrade-stop').exists())
        self.assertFalse(Path('/dev/shm/offline_target_0').exists())

    def test_factory_refuses_existing_runtime_and_shm_reset(self):
        env = self.factory()
        home = Path(env["GATEWAY_HOME"])
        write(home / "config/runtime/device_identity.json", '{"machineCode":"KEEP"}\n')
        result = self.run_script("install-factory-config.sh", env=env)
        self.assertNotEqual(0, result.returncode)
        self.assertIn("not an in-place runtime upgrade", result.stdout)
        self.assertEqual('{"machineCode":"KEEP"}\n', (home / "config/runtime/device_identity.json").read_text())
        self.assert_isolated()
        result = self.run_script("install-factory-config.sh", env={**env, "INSTALL_SYSTEMD": "1", "RESET_SHM": "1"})
        self.assertNotEqual(0, result.returncode)
        self.assertIn("RESET_SHM is unsupported", result.stdout)
        self.assert_isolated()

    def test_service_apply_preserves_v11_and_refuses_v10(self):
        home = self.root / "gateway"
        segment = Path("/dev/shm/gateway_point_store")
        for version in (10, 11):
            with self.subTest(version=version):
                before = struct.pack('<II', 0x4d505354, version) + b'preserve-all-bytes'
                segment.write_bytes(before)
                write(self.log, "")
                result = self.run_script("gateway-services.sh", ["apply"], {"GATEWAY_HOME": str(home)})
                self.assertEqual(before, segment.read_bytes())
                if version == 10:
                    self.assertNotEqual(0, result.returncode)
                    self.assertIn("ABI mismatch", result.stdout)
                    self.assertNotIn("start ", self.log.read_text())
                    self.assertTrue(Path("/run/gateway-health-watchdog/manual-stop").is_file())
                else:
                    self.assertEqual(0, result.returncode, result.stdout)

    def test_service_stop_failure_does_not_start_or_touch_shm(self):
        result = self.run_script("gateway-services.sh", ["apply"],
                                 {"GATEWAY_HOME": str(self.root / "gateway"), "FAIL_SYSTEMCTL_ACTION": "stop"})
        self.assertNotEqual(0, result.returncode)
        self.assertNotIn("start ", self.log.read_text())
        self.assertEqual(b"keep-shm\n", Path("/dev/shm/gateway_point_store_fixture").read_bytes())

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

    def test_factory_archive_extraction_cleanup(self):
        env = self.factory()
        package = self.root / "factory.tar.gz"
        with tarfile.open(package, "w:gz") as archive:
            archive.add(self.root / "source", arcname="gateway-factory-defaults")
        temporary = self.root / "extract"
        result = self.run_script("install-factory-config.sh", env={**env, "FACTORY_PACKAGE": str(package),
                                 "FACTORY_TMP_DIR": str(temporary)})
        self.assertEqual(0, result.returncode, result.stdout)
        self.assertEqual([], list(temporary.iterdir()))
        self.assert_isolated()

    def test_factory_corrupt_archive_fails_closed(self):
        env = self.factory()
        package = self.root / "corrupt.tar.gz"
        write(package, "not a tar archive\n")
        temporary = self.root / "extract"
        result = self.run_script("install-factory-config.sh", env={**env, "FACTORY_PACKAGE": str(package),
                                 "FACTORY_TMP_DIR": str(temporary)})
        self.assertNotEqual(0, result.returncode)
        self.assertFalse((self.root / "gateway/bin/SystemMonitor").exists(), "silently used fallback source")
        self.assertEqual([], list(temporary.iterdir()))
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

    def assert_mixed_case_flag_rejected(self, flag, message):
        self.factory()
        source = self.root / "source"
        write(source / "deploy/production-smoke-test.sh",
              '#!/bin/sh\nprintf "SMOKE_INVOKED\\n" >> "$SYSTEMCTL_LOG"\n')
        package = self.root / "factory.tar.gz"
        with tarfile.open(package, "w:gz") as archive:
            archive.add(source, arcname="gateway-factory-defaults")
        for value in ("tRuE", "yEs", "oN"):
            with self.subTest(flag=flag, value=value):
                home = self.root / value
                result = self.run_script(str(source / "deploy/production-init.sh"),
                                         ["--auto", "--package", package, "--package-profile", "base",
                                          "--no-mqtt-tls", "--no-direct-maintenance"],
                                         {"INSTALL_SYSTEMD": "0", "GATEWAY_HOME": str(home),
                                          "INIT_START_SERVICES": "0", "INIT_RESET_SHM": "0",
                                          "INIT_RUN_SMOKE": "0", flag: value})
                self.assertEqual(2, result.returncode, result.stdout)
                self.assertIn(message, result.stdout)
                self.assertFalse(home.exists(), "guard ran after extraction or installation")
                self.assert_isolated()

    def test_init_mixed_case_start_rejected(self):
        self.assert_mixed_case_flag_rejected("INIT_START_SERVICES", "requires --no-start")

    def test_init_mixed_case_reset_rejected(self):
        self.assert_mixed_case_flag_rejected("INIT_RESET_SHM", "forbids --reset-shm")

    def test_init_mixed_case_smoke_rejected(self):
        self.assert_mixed_case_flag_rejected("INIT_RUN_SMOKE", "requires --no-smoke")

    def test_init_existing_directory_valid_package_rejected(self):
        package = self.root / "empty.tar.gz"
        with tarfile.open(package, "w:gz"):
            pass
        work = self.root / "existing-work"
        write(work / "keep", "another invocation\n")
        result = self.run_script("production-init.sh", ["--auto", "--package", package, "--no-start", "--no-smoke"],
                                 {"INSTALL_SYSTEMD": "0", "INIT_WORK_DIR": str(work),
                                  "GATEWAY_HOME": str(self.root / "gateway")})
        self.assertNotEqual(0, result.returncode)
        self.assertIn("must not already exist", result.stdout)
        self.assertEqual("another invocation\n", (work / "keep").read_text())
        self.assert_isolated()

    def test_init_corrupt_archive_cleans_owned_directory(self):
        package = self.root / "corrupt.tar.gz"
        write(package, "not a tar archive\n")
        work = self.root / "owned-work"
        result = self.run_script("production-init.sh", ["--auto", "--package", package, "--no-start", "--no-smoke"],
                                 {"INSTALL_SYSTEMD": "0", "INIT_WORK_DIR": str(work),
                                  "GATEWAY_HOME": str(self.root / "gateway")})
        self.assertNotEqual(0, result.returncode)
        self.assertFalse(work.exists())
        self.assert_isolated()

    def test_existing_shell_regressions(self):
        for script in ("scada_install_test.sh", "scada_rollback_test.sh",
                       "factory_package_component_version_test.sh"):
            with self.subTest(script=script):
                result = self.run_script("../tools/" + script, env={"TMPDIR": str(self.root)})
                self.assertEqual(0, result.returncode, result.stdout)

    def test_runtime_mode_packages_with_fixture(self):
        self.factory()
        source = self.root / "source"
        for binary in ("ModbusRtu", "Dlt645Driver", "DioDriver", "CanDriver", "IecDriver", "EventEngine",
                       "ComputeEngine", "EmsParityCheck", "EmsClusterCoordinator"):
            write(source / "build-aarch64" / binary, "fixture-not-executable\n")
        package = self.root / "factory.tar.gz"
        with tarfile.open(package, "w:gz") as archive:
            archive.add(source, arcname="gateway-factory-defaults",
                        filter=lambda entry: None if "agc" in entry.name.lower() else entry)
        overlay = self.root / "overlay"
        write(overlay / "build-aarch64/AgcAvcController", "fixture-not-executable\n")
        write(overlay / "config/factory/runtime/apps/agc-avc-service.json", '{"agcAvc":{}}\n')
        write(overlay / "config/factory/runtime/devices/device_agc_avc_virtual.json", '{}\n')
        runtime = self.root / "agc.tar.gz"
        with tarfile.open(runtime, "w:gz") as archive:
            archive.add(overlay, arcname="gateway-factory-defaults")
        result = self.run_script("../tools/runtime_mode_package_test.sh", [package, runtime],
                                 {"TMPDIR": str(self.root)})
        self.assertEqual(0, result.returncode, result.stdout)
        self.assert_isolated()


PROTECTED = ("/etc/default", "/etc/systemd", "/run", "/tmp", "/dev/shm", "/opt")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--evidence", required=True, type=Path)
    parser.add_argument("--parent-mount-ns")
    parser.add_argument("--test", action="append", help="run only named regression methods")
    options = parser.parse_args()
    if os.geteuid() != 0:
        parser.error("requires root for private mounts; never run installers outside the namespace")
    EVIDENCE = options.evidence.resolve()
    REPO = Path(__file__).resolve().parent.parent
    if not options.parent_mount_ns:
        selection = [item for name in options.test or [] for item in ("--test", name)]
        sys.exit(subprocess.call(["unshare", "--mount", "--net", "--ipc", "--fork", sys.executable, str(Path(__file__).resolve()),
                                  "--evidence", str(EVIDENCE), "--parent-mount-ns", os.readlink("/proc/self/ns/mnt"),
                                  *selection]))
    if options.parent_mount_ns == os.readlink("/proc/self/ns/mnt"):
        parser.error("private mount namespace was not created")
    subprocess.run(["mount", "--make-rprivate", "/"], check=True)
    for directory in PROTECTED:
        subprocess.run(["mount", "-t", "tmpfs", "-o", "mode=755", "tmpfs", directory], check=True)
    EVIDENCE.mkdir(parents=True, exist_ok=True)
    with (EVIDENCE / "result.txt").open("w") as output:
        suite = (unittest.TestSuite(InstallIsolationTest(name) for name in options.test) if options.test else
                 unittest.defaultTestLoader.loadTestsFromTestCase(InstallIsolationTest))
        result = unittest.TextTestRunner(stream=output, verbosity=2).run(suite)
    print((EVIDENCE / "result.txt").read_text())
    sys.exit(0 if result.wasSuccessful() else 1)
