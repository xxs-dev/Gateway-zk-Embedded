#!/usr/bin/env python3
"""Offline installer regression: root + private Linux mount/IPC namespaces only."""
import argparse
import hashlib
import io
import json
import os
import copy
import importlib.util
import re
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
  list-unit-files)
    echo 'ky-ems.service enabled'
    [ "${OFFLINE_TEST:-0}" != 1 ] || echo 'compute-engine@.service disabled' ;;
  stop|show)
    case "$*" in *@.service*) echo 'cannot operate on uninstantiated template' >&2; exit 45 ;; esac
    if [ "$1" = show ]; then
      if [ "${OFFLINE_TEST:-0}" = 1 ]; then
        case "$*" in
          *UnitFileState*) echo "${OFFLINE_UNIT_STATE:-disabled}" ;;
          *) echo inactive ;;
        esac
      else echo 4242; fi
    fi ;;
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
        write(home / 'data/cluster-membership.json', '{"schemaVersion":"1.0","clusterId":"OFFLINE_FIXTURE","membershipEpoch":1,"assignments":[{"nodeId":"A","cabinetNo":1},{"nodeId":"B","cabinetNo":2}]}\n')
        write(home / 'data/control-dedup.sqlite', 'current durable dedup\n')
        write(home / 'data/ems-cluster-consensus.json', '{"term":19}\n')
        segments = []
        for i, mode in enumerate(modes):
            source, target = 'offline_source_' + str(i), 'offline_target_' + str(i)
            subprocess.run([str(fixture), mode, source], check=True, stdout=subprocess.PIPE)
            segments.append({'source': source, 'target': target, 'sha256': hashlib.sha256((Path('/dev/shm') / source).read_bytes()).hexdigest()})
        write(home / 'config/runtime/apps/compute.json', json.dumps({'computeEngine': {'sharedMemoryNames': [s['source'] for s in segments], 'outputDefaultSharedMemoryName': segments[0]['source']}, 'emsCluster': {'enabled': True, 'clusterId': 'OFFLINE_FIXTURE', 'expectedMembers': 2, 'maxMembers': 2, 'lockedCabinetNo': 1, 'controlEnabled': False, 'virtualSharedMemoryName': segments[0]['source']}}))
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
        write(EVIDENCE / 'native-fixture-provenance.json', json.dumps({
            'runtimeExecution': 'native CLI only, not AArch64 qualification',
            'migrationCli': components[-1],
            'fixtureSourceSha256': hashlib.sha256((REPO / 'tools/offline_upgrade_fixture.cpp').read_bytes()).hexdigest(),
            'fixtureBinarySha256': hashlib.sha256(fixture.read_bytes()).hexdigest(),
            'runtimeRebuilds': 0}, indent=2) + '\n')
        manifest = self.root / 'program-manifest.json'
        write(manifest, json.dumps({'sourceCommit': 'native-fixture-not-ARM-release', 'components': components}))
        config = home / 'config/runtime'
        sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
        approval = {'schemaVersion': 'offline-shm11-2', 'mode': 'fixed-voter', 'transactionId': 'fixture-offline',
                    'runtimeCompatibility': {'pointStoreAbi': 11, 'clusterProtocol': 2, 'upgradeMode': 'offline-all-participants'},
                    'controlEnabled': False, 'expiresAtUnix': int(time.time()) + 300,
                    'gatewayHome': str(home), 'nodeId': 'A', 'programManifestSha256': sha(manifest),
                    'identitySha256': sha(config / 'device_identity.json'), 'membershipSha256': sha(home / 'data/cluster-membership.json'),
                    'installPaths': {c['target']: 'bin/' + c['target'] for c in components},
                    'installedRuntimeSha256': {'bin/' + n: sha(home / 'bin' / n) for n in ('ComputeEngine', 'MqttDriver')},
                    'configSha256': {str(p.relative_to(config)): sha(p) for p in config.rglob('*') if p.is_file()},
                    'units': ['gateway-services.service', 'gateway-health-watchdog.service', 'ky-ems.service', 'compute-engine@.service', 'compute-engine@fixture.service'],
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
        if action == 'observe':
            command += ['--ready', str(self.root / 'ready.json'), '--ready-sha256', hashlib.sha256((self.root / 'ready.json').read_bytes()).hexdigest()]
        result = subprocess.run(command, env={**self.env, 'OFFLINE_TEST': '1', **(env or {})},
                                stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=120)
        with (EVIDENCE / (self.id().split('.')[-1] + '.log')).open('a') as stream:
            stream.write('action=' + action + ' exit=' + str(result.returncode) + '\n' + result.stdout + '\n')
        return result

    def qt_offline_fixture(self):
        home, approval = self.offline_fixture()
        write(home / 'ky-ems/KY-EMS', 'old real-path qt\n')
        write(home / 'ky-ems/resources/keep.txt', 'keep resources\n')
        manifest = json.loads((self.root / 'program-manifest.json').read_text())
        qt = self.root / 'payload/programs/ky-ems/KY-EMS'
        qt.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile('/usr/bin/true', qt)
        manifest['components'].append({'kind': 'product', 'target': 'LocalDisplayQtEms',
            'archivePath': 'programs/ky-ems/KY-EMS', 'bytes': qt.stat().st_size,
            'sha256': hashlib.sha256(qt.read_bytes()).hexdigest()})
        write(self.root / 'program-manifest.json', json.dumps(manifest))
        approval['programManifestSha256'] = hashlib.sha256((self.root / 'program-manifest.json').read_bytes()).hexdigest()
        approval['installPaths'] = {c['target']: ('ky-ems/KY-EMS' if c['target'] == 'LocalDisplayQtEms' else
                                    'bin/' + c['target']) for c in manifest['components']}
        approval['installedRuntimeSha256']['ky-ems/KY-EMS'] = hashlib.sha256((home / 'ky-ems/KY-EMS').read_bytes()).hexdigest()
        return home, approval

    def test_offline_real_qt_path_roundtrip(self):
        home, approval = self.qt_offline_fixture()
        original = (home / 'ky-ems/KY-EMS').read_bytes()
        result = self.offline_run(approval=approval)
        self.assertEqual(0, result.returncode, result.stdout)
        self.assertEqual(Path('/usr/bin/true').read_bytes(), (home / 'ky-ems/KY-EMS').read_bytes())
        self.assertFalse((home / 'bin/KY-EMS').exists())
        self.assertFalse((home / 'bin/LocalDisplayQtEms').exists())
        self.offline_ready(approval)
        self.assertEqual(0, self.offline_run('observe').returncode)
        self.assertEqual(0, self.offline_run('recover').returncode)
        self.assertEqual(original, (home / 'ky-ems/KY-EMS').read_bytes())
        self.assertEqual('keep resources\n', (home / 'ky-ems/resources/keep.txt').read_text())

    def standalone_fixture(self):
        home, approval = self.offline_fixture(('create', 'create'))
        (home / 'config/runtime/apps/compute.json').unlink()
        provenance = {}
        for source in sorted((REPO / 'config/runtime/apps').glob('*.json')):
            raw = source.read_bytes()
            write(home / 'config/runtime/apps' / source.name, raw.decode('utf-8'))
            provenance[str(source.relative_to(REPO))] = hashlib.sha256(raw).hexdigest()
        # Complete repository non-cluster apps are representative, not live A/B config.
        Path('/dev/shm/offline_source_0').rename('/dev/shm/gateway_point_store')
        approval['segments'][0]['source'] = 'gateway_point_store'
        Path('/dev/shm/offline_source_1').rename('/dev/shm/gateway_point_store_agc_avc')
        approval['segments'][1]['source'] = 'gateway_point_store_agc_avc'
        approval['mode'] = 'standalone'
        approval['offlineLocal'] = dict(approval.pop('offlineVoters')['A'], nodeId=approval['nodeId'])
        approval.pop('membershipSha256')
        (home / 'data/cluster-membership.json').unlink()
        config = home / 'config/runtime'
        approval['configSha256'] = {str(p.relative_to(config)): hashlib.sha256(p.read_bytes()).hexdigest()
                                   for p in config.rglob('*') if p.is_file()}
        write(EVIDENCE / 'standalone-fixture-provenance.json', json.dumps({
            'scope': 'complete repository noncluster app examples; not full live A/B configuration',
            'sources': provenance, 'rosterFabricated': False}, indent=2) + '\n')
        return home, approval

    def test_offline_standalone_full_app_roundtrip(self):
        home, approval = self.standalone_fixture()
        before = {str(p.relative_to(home)): p.read_bytes() for p in (home / 'config/runtime').rglob('*') if p.is_file()}
        result = self.offline_run(approval=approval)
        self.assertEqual(0, result.returncode, result.stdout)
        self.assertFalse((home / 'data/cluster-membership.json').exists())
        self.offline_ready(approval)
        result = self.offline_run('observe')
        self.assertEqual(0, result.returncode, result.stdout)
        result = self.offline_run('recover')
        self.assertEqual(0, result.returncode, result.stdout)
        self.assertEqual(before, {str(p.relative_to(home)): p.read_bytes() for p in (home / 'config/runtime').rglob('*') if p.is_file()})
        self.assertFalse((home / 'data/cluster-membership.json').exists())
        self.assertNotIn('start ems-cluster@', self.log.read_text())

    def test_offline_program_paths_reject_unsafe_or_uncovered(self):
        home, approval = self.qt_offline_fixture()
        for path in ('/tmp/KY-EMS', '../KY-EMS', 'ky-ems/../bin/KY-EMS', 'ky-ems//KY-EMS',
                     'bin/MqttDriver', 'data/KY-EMS'):
            broken = copy.deepcopy(approval)
            broken['installPaths']['LocalDisplayQtEms'] = path
            with self.subTest(path=path):
                result = self.offline_run(approval=broken)
                self.assertNotEqual(0, result.returncode, result.stdout)
                self.assertFalse((home / 'data/runtime-upgrade-stop').exists())
        broken = copy.deepcopy(approval)
        broken['installedRuntimeSha256']['ky-ems/KY-EMS'] = '0' * 64
        self.assertNotEqual(0, self.offline_run(approval=broken).returncode)
        broken = copy.deepcopy(approval)
        del broken['installedRuntimeSha256']['ky-ems/KY-EMS']
        self.assertNotEqual(0, self.offline_run(approval=broken).returncode)
        hidden = home / 'ky-ems/resources/unapproved-runtime'
        shutil.copyfile('/usr/bin/true', hidden)
        result = self.offline_run(approval=approval)
        self.assertNotEqual(0, result.returncode)
        self.assertIn('installed runtime absent', result.stdout)
        hidden.unlink()
        link = home / 'ky-ems/resources/link'
        link.symlink_to(home / 'ky-ems/KY-EMS')
        result = self.offline_run(approval=approval)
        self.assertNotEqual(0, result.returncode)
        self.assertIn('symlink', result.stdout)
        self.assertFalse((home / 'data/runtime-upgrade-stop').exists())

    def test_offline_program_duplicate_destinations_refused(self):
        home, approval = self.qt_offline_fixture()
        manifest = json.loads((self.root / 'program-manifest.json').read_text())
        duplicate = copy.deepcopy(manifest['components'][-1])
        duplicate['target'] = 'KY-EMS'
        manifest['components'].append(duplicate)
        write(self.root / 'program-manifest.json', json.dumps(manifest))
        approval['programManifestSha256'] = hashlib.sha256((self.root / 'program-manifest.json').read_bytes()).hexdigest()
        approval['installPaths']['KY-EMS'] = 'ky-ems/KY-EMS'
        result = self.offline_run(approval=approval)
        self.assertNotEqual(0, result.returncode)
        self.assertIn('duplicate program destination', result.stdout)
        self.assertFalse((home / 'data/runtime-upgrade-stop').exists())

    def test_offline_observe_rejects_added_runtime_path(self):
        home, approval = self.qt_offline_fixture()
        self.assertEqual(0, self.offline_run(approval=approval).returncode)
        self.offline_ready(approval)
        extra = home / 'ky-ems/resources/extra-elf'
        shutil.copyfile('/usr/bin/true', extra)
        result = self.offline_run('observe')
        self.assertNotEqual(0, result.returncode, result.stdout)
        self.assertIn('installed runtime absent', result.stdout)
        self.assertNotIn('\nstart ', '\n' + self.log.read_text())

    def test_offline_standalone_requires_explicit_mode_and_local_evidence(self):
        home, approval = self.standalone_fixture()
        cases = []
        for field, value in (('mode', None), ('mode', 'fixed-voter'), ('mode', 'other'),
                             ('schemaVersion', 'offline-shm11-1'), ('offlineLocal', {}),
                             ('offlineVoters', {}), ('membershipSha256', 'a' * 64)):
            broken = copy.deepcopy(approval)
            if value is None:
                broken.pop(field)
            else:
                broken[field] = value
            cases.append(broken)
        for field in ('controlDisabled', 'participantsStopped', 'restartInhibited'):
            broken = copy.deepcopy(approval)
            broken['offlineLocal'][field] = False
            cases.append(broken)
        broken = copy.deepcopy(approval)
        broken['offlineLocal']['nodeId'] = 'FOREIGN'
        cases.append(broken)
        for broken in cases:
            with self.subTest(approval=broken):
                self.assertNotEqual(0, self.offline_run(approval=broken).returncode)
                self.assertFalse((home / 'data/runtime-upgrade-stop').exists())
        config = home / 'config/runtime/apps/monitor-service.json'
        original = json.loads(config.read_text())
        for enabled in (True, 'true', 1):
            changed = copy.deepcopy(original)
            changed['emsCluster'] = {'enabled': enabled, 'controlEnabled': False}
            write(config, json.dumps(changed))
            approval['configSha256']['apps/monitor-service.json'] = hashlib.sha256(config.read_bytes()).hexdigest()
            self.assertNotEqual(0, self.offline_run(approval=approval).returncode)
            self.assertFalse((home / 'data/runtime-upgrade-stop').exists())

    def test_offline_standalone_observe_rejects_cluster_and_physical(self):
        home, approval = self.standalone_fixture()
        approval['units'].extend(['ems-cluster@fixture.service', 'modbus-rtu@fixture.service'])
        write(home / 'data/cluster-membership.json', 'preserved disabled historical roster\n')
        result = self.offline_run(approval=approval)
        self.assertEqual(0, result.returncode, result.stdout)
        for unit in ('ems-cluster@fixture.service', 'modbus-rtu@fixture.service'):
            ready = self.offline_ready(approval)
            ready['startUnits'] = [unit]
            write(self.root / 'ready.json', json.dumps(ready))
            result = self.offline_run('observe')
            self.assertNotEqual(0, result.returncode, result.stdout)
            self.assertIn('physical participants remain inhibited', result.stdout)
        ready = self.offline_ready(approval)
        ready['mode'] = 'fixed-voter'
        write(self.root / 'ready.json', json.dumps(ready))
        self.assertNotEqual(0, self.offline_run('observe').returncode)
        ready = self.offline_ready(approval)
        ready['local']['stateSha256'] = 'a' * 64
        write(self.root / 'ready.json', json.dumps(ready))
        self.assertNotEqual(0, self.offline_run('observe').returncode)
        self.assertNotIn('\nstart ', '\n' + self.log.read_text())
        self.assertEqual(0, self.offline_run('recover').returncode)
        self.assertEqual('preserved disabled historical roster\n', (home / 'data/cluster-membership.json').read_text())

    def test_offline_standalone_disabled_absent_ems_roundtrip(self):
        home, approval = self.standalone_fixture()
        config = home / 'config/runtime/apps/mqtt-service.json'
        document = json.loads(config.read_text())
        document['emsCluster'] = {'enabled': False, 'controlEnabled': False,
                                  'virtualSharedMemoryName': 'ems_cluster_store'}
        write(config, json.dumps(document))
        approval['configSha256']['apps/mqtt-service.json'] = hashlib.sha256(config.read_bytes()).hexdigest()
        self.assertFalse(Path('/dev/shm/ems_cluster_store').exists())
        self.assertNotIn('ems_cluster_store', {item['source'] for item in approval['segments']})
        result = self.offline_run(approval=approval)
        self.assertEqual(0, result.returncode, result.stdout)
        self.assertFalse(Path('/dev/shm/ems_cluster_store').exists())
        self.assertEqual('ems_cluster_store', json.loads(config.read_text())['emsCluster']['virtualSharedMemoryName'])
        self.assertFalse((home / 'data/cluster-membership.json').exists())
        self.offline_ready(approval)
        self.assertEqual(0, self.offline_run('observe').returncode)
        self.assertNotIn('ems-cluster', self.log.read_text())
        self.assertEqual(0, self.offline_run('recover').returncode)
        self.assertFalse(Path('/dev/shm/ems_cluster_store').exists())

    def test_offline_unreferenced_default_stopped_only(self):
        home, approval = self.offline_fixture(('create', 'create'))
        Path('/dev/shm/offline_source_0').rename('/dev/shm/gateway_point_store')
        approval['segments'][0]['source'] = 'gateway_point_store'
        config = home / 'config/runtime/apps/compute.json'
        document = json.loads(config.read_text())
        only_referenced = approval['segments'][1]['source']
        document['computeEngine']['sharedMemoryNames'] = [only_referenced]
        document['computeEngine']['outputDefaultSharedMemoryName'] = only_referenced
        document['emsCluster']['virtualSharedMemoryName'] = only_referenced
        write(config, json.dumps(document))
        approval['configSha256']['apps/compute.json'] = hashlib.sha256(config.read_bytes()).hexdigest()
        result = self.offline_run(approval=approval)
        self.assertEqual(0, result.returncode, result.stdout)
        state = json.loads((self.root / 'transaction/state.json').read_text())
        self.assertEqual('UPGRADED_STOPPED', state['phase'])
        self.assertEqual(approval['segments'][0]['sha256'],
                         hashlib.sha256(Path('/dev/shm/gateway_point_store').read_bytes()).hexdigest())
        self.assertEqual(10, struct.unpack('<II', Path('/dev/shm/gateway_point_store').read_bytes()[:8])[1])
        self.assertTrue((self.root / 'transaction/gateway_point_store.v10.bak').is_file())
        self.assertTrue(Path('/dev/shm/offline_target_0').is_file())
        subprocess.run([str(EVIDENCE / 'offline-upgrade-fixture'), 'verify', 'offline_target_0'], check=True)
        self.offline_ready(approval)
        result = self.offline_run('observe')
        self.assertNotEqual(0, result.returncode, result.stdout)
        self.assertIn('unreferenced default SHM', result.stdout)
        self.assertNotIn('\nstart ', '\n' + self.log.read_text())
        self.assertEqual(0, self.offline_run('recover').returncode)

    def test_offline_arbitrary_extra_segment_rejected(self):
        home, approval = self.offline_fixture()
        approval['segments'].append({'source': 'offline_unreferenced_extra',
                                     'target': 'offline_extra_target', 'sha256': 'a' * 64})
        result = self.offline_run(approval=approval)
        self.assertNotEqual(0, result.returncode, result.stdout)
        self.assertIn('all SHM references must be explicit and covered exactly', result.stdout)
        self.assertFalse((home / 'data/runtime-upgrade-stop').exists())

    def test_offline_standalone_ems_reference_boundaries(self):
        home, approval = self.standalone_fixture()
        config = home / 'config/runtime/apps/mqtt-service.json'
        original = json.loads(config.read_text())
        for enabled, extra, expected in ((True, False, 'standalone cannot bypass'),
                                         ('false', False, 'ambiguous EMS'),
                                         (None, False, 'all SHM references'),
                                         (False, True, 'all SHM references')):
            with self.subTest(enabled=enabled, other_reader=extra):
                document = copy.deepcopy(original)
                cluster = {'controlEnabled': False, 'virtualSharedMemoryName': 'ems_cluster_store'}
                if enabled is not None:
                    cluster['enabled'] = enabled
                document['emsCluster'] = cluster
                if extra:
                    document['mqttDriver']['sharedMemoryNames'].append('ems_cluster_store')
                write(config, json.dumps(document))
                approval['configSha256']['apps/mqtt-service.json'] = hashlib.sha256(config.read_bytes()).hexdigest()
                result = self.offline_run(approval=approval)
                self.assertNotEqual(0, result.returncode, result.stdout)
                self.assertIn(expected, result.stdout)
                self.assertFalse((home / 'data/runtime-upgrade-stop').exists())
                self.assertFalse(Path('/dev/shm/ems_cluster_store').exists())

    def test_startup_guard_disabled_ems_reference_scope(self):
        home = self.root / 'gateway'
        config = home / 'config/runtime/apps/mqtt-service.json'
        segment = Path('/dev/shm/ems_cluster_store')
        segment.write_bytes(struct.pack('<II', 0x4d505354, 10))
        document = {'emsCluster': {'enabled': False, 'virtualSharedMemoryName': 'ems_cluster_store'},
                    'cameraService': {'sharedMemoryName': ''}}
        command = ['python3', '-B', str(REPO / 'deploy/runtime-upgrade-guard.py'), 'startup', str(home)]
        def startup():
            write(config, json.dumps(document))
            return subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                  text=True, timeout=10)
        self.assertEqual(0, startup().returncode)
        document['emsCluster']['enabled'] = True
        self.assertIn('point-store ABI mismatch', startup().stdout)
        document['emsCluster']['enabled'] = False
        document['mqttDriver'] = {'enabled': True, 'sharedMemoryName': 'ems_cluster_store'}
        self.assertIn('point-store ABI mismatch', startup().stdout)
        del document['mqttDriver']
        document['cameraService']['sharedMemoryName'] = 'ems_cluster_store'
        self.assertIn('point-store ABI mismatch', startup().stdout)

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
        write(EVIDENCE / 'fixture-upgraded-state.json', json.dumps(state, indent=2) + '\n')
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
        write(EVIDENCE / 'fixture-recovered-state.json', (self.root / 'transaction/state.json').read_text())

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

    def offline_ready(self, approval):
        state_pin = hashlib.sha256((self.root / 'transaction/state.json').read_bytes()).hexdigest()
        ready = {'schemaVersion': 'offline-shm11-observe-2', 'mode': approval['mode'], 'controlEnabled': False,
                 'transactionId': approval['transactionId'], 'expiresAtUnix': int(time.time()) + 300,
                 'approvalSha256': hashlib.sha256((self.root / 'approval.json').read_bytes()).hexdigest(),
                 'programManifestSha256': approval['programManifestSha256'], 'stateSha256': state_pin,
                 'startUnits': ['compute-engine@fixture.service'],
                 'voters': {n: {'phase': 'UPGRADED_STOPPED', 'controlEnabled': False,
                                'programManifestSha256': approval['programManifestSha256'], 'stateSha256': state_pin} for n in ('A', 'B')}}
        if approval['mode'] == 'standalone':
            ready['local'] = dict(ready.pop('voters')['A'], nodeId=approval['nodeId'])
        write(self.root / 'ready.json', json.dumps(ready))
        return ready

    def real_offline_fixture(self, scope='r4'):
        home, approval = self.offline_fixture()
        actual = REPO.parent.parent / 'acceptance-evidence-20260924/tests/e2e/GW-20260809-002/ems-shadow-20260924/pair-recovery-r4/actual-inputs'
        roster_raw = (actual / 'membership.json').read_bytes()
        self.assertEqual('da8b016f579bdb6436d9569bf8234a26f4ac1c90d298840bb4080b554537361c', hashlib.sha256(roster_raw).hexdigest())
        roster = json.loads(roster_raw)
        app = json.loads((actual / 'A/app.public.json').read_text())
        pinned = json.loads((actual / 'manifest.json').read_text())['files']
        self.assertEqual(pinned['A/app.public.json']['sha256'], hashlib.sha256((actual / 'A/app.public.json').read_bytes()).hexdigest())
        self.assertEqual('EMS_SHADOW_20260924_RECOVERY_R4', app['emsCluster']['clusterId'])
        if scope == 's2':
            def relocate(value):
                if isinstance(value, dict):
                    return {k: relocate(v) for k, v in value.items()}
                if isinstance(value, list):
                    return [relocate(v) for v in value]
                if isinstance(value, str):
                    for directory in ('data', 'run'):
                        old = '/opt/modbus-gateway/' + directory + '/ems-shadow-20260924-r4'
                        if value.startswith(old + '/'):
                            return '/opt/modbus-gateway/' + directory + '/ems-sustained-20260924-s2' + value[len(old):]
                    if value == 'EMS_SHADOW_20260924_RECOVERY_R4':
                        return 'EMS_SUSTAINED_20260924_S2'
                return value
            app, roster = relocate(app), relocate(roster)
            roster_raw = (json.dumps(roster, separators=(',', ':'), allow_nan=False) + '\n').encode()
            app_raw = (json.dumps(app, separators=(',', ':'), allow_nan=False) + '\n').encode()
            reviewed = json.loads((actual.parent.parent / 'sustained-20260924/sustained-review-s2.json').read_text())['payloadHashes']['A']
            self.assertEqual(reviewed['app.public.json'], hashlib.sha256(app_raw).hexdigest())
            self.assertEqual(reviewed['membership.json'], hashlib.sha256(roster_raw).hexdigest())
        write(EVIDENCE / ('real-fixture-provenance-' + scope + '.json'), json.dumps({'scope': scope,
              'rosterSha256': hashlib.sha256(roster_raw).hexdigest(),
              'sourceAppSha256': hashlib.sha256((actual / 'A/app.public.json').read_bytes()).hexdigest(),
              'adaptations': ['controlEnabled=false', 'canonical private identity and membership paths', 'native fixture SHM bytes']}, indent=2) + '\n')
        app['emsCluster']['controlEnabled'] = False
        app['emsCluster']['membershipFile'] = str(home / 'data/cluster-membership.json')
        app['identityConfigFile'] = str(home / 'config/runtime/device_identity.json')
        write(home / 'config/runtime/apps/compute.json', json.dumps(app))
        identity_raw = (actual / 'A/identity.json').read_bytes()
        (home / 'config/runtime/device_identity.json').write_bytes(identity_raw)
        (home / 'data/cluster-membership.json').write_bytes(roster_raw)
        node = json.loads(identity_raw)['machineCode']
        approval['nodeId'] = node
        approval['identitySha256'] = hashlib.sha256(identity_raw).hexdigest()
        approval['membershipSha256'] = hashlib.sha256(roster_raw).hexdigest()
        approval['offlineVoters'] = {v['nodeId']: copy.deepcopy(approval['offlineVoters']['A']) for v in roster['assignments']}
        config = home / 'config/runtime'
        approval['configSha256'] = {str(p.relative_to(config)): hashlib.sha256(p.read_bytes()).hexdigest() for p in config.rglob('*') if p.is_file()}
        old = approval['segments'][0]
        source = Path('/dev/shm/ems_shadow_20260924_pair')
        Path('/dev/shm/offline_source_0').rename(source)
        old['source'] = source.name
        return home, approval, roster, app

    def test_offline_real_r4_roster_and_app(self):
        self.real_offline_roundtrip('r4')

    def test_offline_real_s2_roster_and_app(self):
        self.real_offline_roundtrip('s2')

    def real_offline_roundtrip(self, scope):
        home, approval, roster, app = self.real_offline_fixture(scope)
        before_identity = (home / 'config/runtime/device_identity.json').read_bytes()
        result = self.offline_run(approval=approval)
        self.assertEqual(0, result.returncode, result.stdout)
        installed = json.loads((home / 'config/runtime/apps/compute.json').read_text())
        self.assertEqual('offline_target_0', installed['computeEngine']['outputDefaultSharedMemoryName'])
        self.assertEqual('offline_target_0', installed['emsCluster']['virtualSharedMemoryName'])
        self.assertEqual('offline_target_0', installed['mqttDriver']['sharedMemoryName'])
        self.assertEqual(['offline_target_0'], installed['computeEngine']['sharedMemoryNames'])
        self.assertTrue(all(p['sharedMemoryName'] == 'offline_target_0' for p in installed['computeEngine']['rules'][0]['outputs']))
        self.assertNotIn('ems_shadow_20260924_pair', json.dumps(installed))
        self.offline_ready(approval)
        ready = json.loads((self.root / 'ready.json').read_text())
        receipt = ready['voters']['A']
        ready['voters'] = {v['nodeId']: copy.deepcopy(receipt) for v in roster['assignments']}
        write(self.root / 'ready.json', json.dumps(ready))
        result = self.offline_run('observe')
        self.assertEqual(0, result.returncode, result.stdout)
        result = self.offline_run('recover')
        self.assertEqual(0, result.returncode, result.stdout)
        self.assertEqual(app, json.loads((home / 'config/runtime/apps/compute.json').read_text()))
        self.assertEqual(before_identity, (home / 'config/runtime/device_identity.json').read_bytes())

    def test_offline_real_roster_invalid_assignments_refused(self):
        home, approval, roster, app = self.real_offline_fixture()
        cases = []
        for field, value in (('nodeId', roster['assignments'][0]['nodeId']), ('cabinetNo', 1), ('cabinetNo', True), ('cabinetNo', 0), ('cabinetNo', 3)):
            broken = copy.deepcopy(roster)
            broken['assignments'][1][field] = value
            cases.append(broken)
        broken = copy.deepcopy(roster)
        broken['assignments'][0]['nodeId'] = 'FOREIGN'
        cases.append(broken)
        broken = copy.deepcopy(roster)
        broken['assignments'].pop()
        cases.append(broken)
        for field, value in (('schemaVersion', '2.0'), ('membershipEpoch', 0), ('membershipEpoch', True), ('clusterId', 'FOREIGN')):
            broken = copy.deepcopy(roster)
            broken[field] = value
            cases.append(broken)
        broken = copy.deepcopy(roster)
        broken['members'] = broken.pop('assignments')
        cases.append(broken)
        for broken in cases:
            with self.subTest(roster=broken):
                write(home / 'data/cluster-membership.json', json.dumps(broken))
                approval['membershipSha256'] = hashlib.sha256((home / 'data/cluster-membership.json').read_bytes()).hexdigest()
                result = self.offline_run(approval=approval)
                self.assertNotEqual(0, result.returncode, result.stdout)
                self.assertFalse((home / 'data/runtime-upgrade-stop').exists())
                self.assertFalse(Path('/dev/shm/offline_target_0').exists())

    def test_offline_real_roster_config_and_hash_mismatch_refused(self):
        home, approval, roster, app = self.real_offline_fixture()
        write(home / 'data/cluster-membership.json', json.dumps(roster))
        result = self.offline_run(approval=approval)
        self.assertNotEqual(0, result.returncode)
        self.assertIn('SHA256 mismatch', result.stdout)
        approval['membershipSha256'] = hashlib.sha256((home / 'data/cluster-membership.json').read_bytes()).hexdigest()
        for changes in ({'expectedMembers': 3, 'maxMembers': 3}, {'lockedCabinetNo': 2},
                        {'expectedMembers': True}, {'membershipFile': '/unsupported/membership.json'}):
            changed = copy.deepcopy(app)
            changed['emsCluster'].update(changes)
            config = home / 'config/runtime/apps/compute.json'
            write(config, json.dumps(changed))
            approval['configSha256']['apps/compute.json'] = hashlib.sha256(config.read_bytes()).hexdigest()
            result = self.offline_run(approval=approval)
            self.assertNotEqual(0, result.returncode, result.stdout)
            self.assertFalse((home / 'data/runtime-upgrade-stop').exists())

    def test_offline_observe_starts_only_observers_and_recovers(self):
        home, approval = self.offline_fixture()
        result = self.offline_run()
        self.assertEqual(0, result.returncode, result.stdout)
        self.offline_ready(approval)
        result = self.offline_run('observe')
        self.assertEqual(0, result.returncode, result.stdout)
        self.assertIn('OBSERVING_CONTROL_DISABLED', result.stdout)
        self.assertIn('start compute-engine@fixture.service', self.log.read_text())
        self.assertNotIn('start gateway-services.service', self.log.read_text())
        for unit in approval['units']:
            self.assertTrue((Path('/etc/systemd/system') / (unit + '.d/90-offline-fixture-offline.conf')).exists())
        self.assertTrue((home / 'data/runtime-upgrade-stop').exists())
        self.assertNotEqual(0, self.offline_run('observe').returncode, 'ready receipt must not be replayed')
        result = self.offline_run('recover')
        self.assertEqual(0, result.returncode, result.stdout)

    def test_offline_observe_start_failure_refences(self):
        home, approval = self.offline_fixture()
        self.assertEqual(0, self.offline_run().returncode)
        self.offline_ready(approval)
        result = self.offline_run('observe', env={'FAIL_SYSTEMCTL_ACTION': 'start'})
        self.assertNotEqual(0, result.returncode)
        self.assertIn('stop compute-engine@fixture.service', self.log.read_text())
        self.assertTrue(Path('/etc/systemd/system/compute-engine@.service.d/90-offline-fixture-offline.conf').is_file())
        self.assertTrue((home / 'data/runtime-upgrade-stop').is_file())

    def test_offline_observe_rejects_masked_units_and_foreign_voters(self):
        _, approval = self.offline_fixture()
        result = self.offline_run(env={'OFFLINE_UNIT_STATE': 'masked'})
        self.assertEqual(0, result.returncode, result.stdout)
        ready = self.offline_ready(approval)
        result = self.offline_run('observe', env={'OFFLINE_UNIT_STATE': 'masked'})
        self.assertNotEqual(0, result.returncode)
        self.assertIn('was masked', result.stdout)
        del ready['voters']['B']
        write(self.root / 'ready.json', json.dumps(ready))
        result = self.offline_run('observe')
        self.assertNotEqual(0, result.returncode)
        self.assertIn('all fixed voters', result.stdout)
        self.assertNotIn('\nstart ', '\n' + self.log.read_text())

    def test_offline_output_only_reference_and_qt_alias(self):
        home, approval = self.offline_fixture(('create', 'create'))
        config = home / 'config/runtime/apps/compute.json'
        app = json.loads(config.read_text())
        app['computeEngine'] = {'outputDefaultSharedMemoryName': 'offline_source_1'}
        write(config, json.dumps(app))
        approval['configSha256']['apps/compute.json'] = hashlib.sha256(config.read_bytes()).hexdigest()
        write(home / 'bin/KY-EMS', 'old qt\n')
        approval['installedRuntimeSha256']['bin/KY-EMS'] = hashlib.sha256((home / 'bin/KY-EMS').read_bytes()).hexdigest()
        qt = self.root / 'payload/programs/qt/KY-SCADA'
        qt.parent.mkdir(parents=True)
        shutil.copyfile('/usr/bin/true', qt)
        manifest = json.loads((self.root / 'program-manifest.json').read_text())
        manifest['components'].append({'kind': 'product', 'target': 'LocalDisplayQtEms', 'archivePath': 'programs/qt/KY-SCADA',
                                       'bytes': qt.stat().st_size, 'sha256': hashlib.sha256(qt.read_bytes()).hexdigest()})
        write(self.root / 'program-manifest.json', json.dumps(manifest))
        approval['programManifestSha256'] = hashlib.sha256((self.root / 'program-manifest.json').read_bytes()).hexdigest()
        result = self.offline_run(approval=approval)
        self.assertNotEqual(0, result.returncode)
        self.assertIn('exact installPaths product mapping', result.stdout)
        approval['installPaths']['LocalDisplayQtEms'] = 'bin/KY-EMS'
        result = self.offline_run(approval=approval)
        self.assertEqual(0, result.returncode, result.stdout)
        self.assertEqual(qt.read_bytes(), (home / 'bin/KY-EMS').read_bytes())
        self.assertFalse((home / 'bin/LocalDisplayQtEms').exists())
        self.assertIn('offline_target_0', config.read_text())
        self.assertIn('offline_target_1', config.read_text())
        result = self.offline_run('recover')
        self.assertEqual(0, result.returncode, result.stdout)
        self.assertEqual('old qt\n', (home / 'bin/KY-EMS').read_text())

    def test_offline_shm_keys_match_runtime_parsers(self):
        sys.dont_write_bytecode = True
        spec = importlib.util.spec_from_file_location('offline_test', str(REPO / 'deploy/offline-runtime-upgrade.py'))
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        keys = set()
        for source in ('src/config_loader.cpp', 'src/scada_project_loader.cpp'):
            keys.update(re.findall(r'"([A-Za-z]*[sS]haredMemoryNames?)"', (REPO / source).read_text()))
        self.assertEqual(keys, module.guard.SINGLE_SHM_KEYS | module.guard.MULTI_SHM_KEYS)
        for key in keys:
            original = {key: ['old'] if key in module.guard.MULTI_SHM_KEYS else 'old'}
            self.assertEqual({'old'}, module.guard.configured_names(original))
            updated = module.switch_names(original, {'old': 'new'})
            self.assertEqual({'new'}, module.guard.configured_names(updated))
            module.no_old_references(updated, {'old': 'new'})
        for invalid in ({'sharedMemoryNames': ['old', 1]}, {'sharedMemoryName': 1},
                        {'outputDefaultSharedMemoryName': '//old'}, {'unknownSharedMemoryName': 'old'}):
            with self.assertRaises(ValueError):
                module.guard.configured_names(invalid)
        for enabled in (True, 'false', 0, None):
            cluster = {'virtualSharedMemoryName': 'ems_cluster_store'}
            if enabled is not None:
                cluster['enabled'] = enabled
            self.assertEqual({'ems_cluster_store'}, module.guard.configured_names({'emsCluster': cluster}))
        disabled = {'emsCluster': {'enabled': False, 'virtualSharedMemoryName': 'ems_cluster_store'}}
        self.assertEqual(set(), module.guard.configured_names(disabled))
        self.assertEqual({'ems_cluster_store'}, module.guard.configured_names({
            **disabled, 'mqttDriver': {'sharedMemoryName': 'ems_cluster_store'}}))
        self.assertEqual({'ems_cluster_store'}, module.guard.configured_names({
            'cameraService': {'enabled': False, 'sharedMemoryName': 'ems_cluster_store'}}))
        with self.assertRaises(ValueError):
            module.guard.configured_names({'emsCluster': {
                'enabled': False, 'virtualSharedMemoryName': '//invalid'}})
        with self.assertRaises(ValueError):
            module.no_old_references({'unrecognizedStore': 'old'}, {'old': 'new'})
        write(EVIDENCE / 'runtime-shm-key-coverage.json', json.dumps({'runtimeParserKeys': sorted(keys),
              'guardKeys': sorted(module.guard.SINGLE_SHM_KEYS | module.guard.MULTI_SHM_KEYS)}, indent=2) + '\n')

    def test_offline_observe_rejects_physical_and_changed_inputs(self):
        home, approval = self.offline_fixture()
        approval['units'].append('modbus-rtu@fixture.service')
        self.assertEqual(0, self.offline_run(approval=approval).returncode)
        ready = self.offline_ready(approval)
        ready['startUnits'] = ['modbus-rtu@fixture.service']
        write(self.root / 'ready.json', json.dumps(ready))
        result = self.offline_run('observe')
        self.assertNotEqual(0, result.returncode)
        self.assertIn('physical participants remain inhibited', result.stdout)
        self.offline_ready(approval)
        config = home / 'config/runtime/apps/compute.json'
        changed = json.loads(config.read_text())
        changed['emsCluster']['controlEnabled'] = True
        write(config, json.dumps(changed))
        result = self.offline_run('observe')
        self.assertNotEqual(0, result.returncode)
        self.assertIn('configuration changed', result.stdout)
        self.assertNotIn('\nstart ', '\n' + self.log.read_text())

    def test_full_factory_contains_migration_cli_and_offline_scripts(self):
        env = self.factory()
        source = self.root / 'source'
        for name in ('ModbusRtu', 'Dlt645Driver', 'DioDriver', 'CanDriver', 'IecDriver', 'EventEngine',
                     'ComputeEngine', 'EmsParityCheck', 'EmsClusterCoordinator', 'memory_point_store_migrate'):
            write(source / 'build-aarch64' / name, 'fixture-not-executable\n')
        archive_path = self.root / 'full-factory.tar.gz'
        result = self.run_script(str(source / 'deploy/build-factory-package.sh'), ['--profile', 'full', '--out', archive_path],
                                 {'TMPDIR': str(self.root), 'EDGE_PACKAGE_BUILD_DIR': str(source / 'build-aarch64')})
        self.assertEqual(0, result.returncode, result.stdout)
        with tarfile.open(archive_path) as archive:
            names = set(archive.getnames())
            for name in ('build-aarch64/memory_point_store_migrate', 'deploy/offline-runtime-upgrade.py', 'deploy/runtime-upgrade-guard.py'):
                self.assertIn('gateway-factory-defaults/' + name, names)
            manifest = json.load(archive.extractfile('gateway-factory-defaults/edge-package-manifest.json'))
            self.assertIn('memory_point_store_migrate', {c['binary'] for c in manifest['components']})
        result = self.run_script('install-factory-config.sh', env={**env, 'PACKAGE_PROFILE': 'full'})
        self.assertEqual(0, result.returncode, result.stdout)
        self.assertTrue((self.root / 'gateway/bin/memory_point_store_migrate').is_file())

    def test_offline_pairing_provenance(self):
        scripts = ['build-factory-package.sh', 'gateway-services.sh', 'install-factory-config.sh',
                   'ota-apply.sh', 'ota-rollback.sh', 'upgrade-legacy-runtime-v9.sh',
                   'runtime-upgrade-guard.py', 'offline-runtime-upgrade.py']
        rows = []
        for name in scripts:
            raw = (REPO / 'deploy' / name).read_bytes()
            self.assertNotIn(b'\r', raw, name + ' must use LF')
            rows.append({'path': 'deploy/' + name, 'bytes': len(raw), 'sha256': hashlib.sha256(raw).hexdigest()})
        write(EVIDENCE / 'paired-deploy-delta.json', json.dumps({
            'qualifiedRuntimeSource': '9ccfce8822a2504524c29d9505179ab9e94aa775',
            'approvedProgramManifestSha256': '75c7018d4d78d93e67f8239c041b5f1003d460f0266f549e0c25053cc9795ca6',
            'runtimeElfRebuildRequired': False, 'deviceMigrationExecuted': False,
            'files': rows}, indent=2) + '\n')

    def test_offline_r5_actual_twenty_component_preflight(self):
        home, approval = self.offline_fixture()
        coordinator = Path('/mnt/d/workspace/GatewaySuite-workspaces/realese1.0/coordinator/evidence/GW-20260809-002/ems-cluster-20260924/stage16-recovery-arm/program-manifest.json')
        manifest_pin = '75c7018d4d78d93e67f8239c041b5f1003d460f0266f549e0c25053cc9795ca6'
        self.assertEqual(manifest_pin, hashlib.sha256(coordinator.read_bytes()).hexdigest())
        manifest = json.loads(coordinator.read_text())
        raw = REPO.parent.parent / 'acceptance-evidence-20260924/evidence/raw/GW-20260809-002'
        archives = [(raw / 'helper-r4-20260924T102700Z/r3-candidate-cd6b529.recovered.tar.gz', '0232f029a66b9df5a502ee8d10e3e24eac807edea82ee7346ae1042c8901a2d9'),
                    (raw / 'arm-r5-20260924/r5-reelection-9ccfce8-delta.tar.gz', '2dc8582535ea8358c839eec95ba509df2001915b91989132793164866ca3a652')]
        products = [c for c in manifest['components'] if c['kind'] == 'product']
        self.assertEqual(20, len(products))
        payload = self.root / 'r5-payload'
        for archive_path, pin in archives:
            self.assertEqual(pin, hashlib.sha256(archive_path.read_bytes()).hexdigest())
            with tarfile.open(archive_path) as archive:
                members = {m.name: m for m in archive.getmembers()}
                for component in products:
                    member = members.get(component['archivePath'])
                    if member is not None:
                        self.assertTrue(member.isfile())
                        data = archive.extractfile(member).read()
                        if hashlib.sha256(data).hexdigest() == component['sha256']:
                            dest = payload / component['archivePath']
                            dest.parent.mkdir(parents=True, exist_ok=True)
                            dest.write_bytes(data)
        write(home / 'bin/KY-EMS', 'old qt\n')
        approval['installPaths'] = {c['target']: ('bin/KY-EMS' if c['target'] == 'LocalDisplayQtEms' else
                                    'bin/' + c['target']) for c in products}
        approval['installedRuntimeSha256']['bin/KY-EMS'] = hashlib.sha256((home / 'bin/KY-EMS').read_bytes()).hexdigest()
        approval['programManifestSha256'] = manifest_pin
        write(self.root / 'approval.json', json.dumps(approval))
        approval_pin = hashlib.sha256((self.root / 'approval.json').read_bytes()).hexdigest()
        command = ['python3', '-B', '-c', 'import importlib.util,types,pathlib,sys; s=importlib.util.spec_from_file_location("offline",sys.argv[1]); m=importlib.util.module_from_spec(s); s.loader.exec_module(m); a=types.SimpleNamespace(approval=pathlib.Path(sys.argv[2]),approval_sha256=sys.argv[3],manifest=pathlib.Path(sys.argv[4]),payload=pathlib.Path(sys.argv[5])); p,doc,components,home=m.read_inputs(a); print("R5 20-product preflight PASS; Qt="+m.install_paths(components,p)["LocalDisplayQtEms"]);', str(REPO / 'deploy/offline-runtime-upgrade.py'), str(self.root / 'approval.json'), approval_pin, str(coordinator), str(payload)]
        result = subprocess.run(command, env={**self.env, 'OFFLINE_TEST': '1'}, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=30)
        write(EVIDENCE / 'r5-twenty-component-preflight.log', result.stdout)
        self.assertEqual(0, result.returncode, result.stdout)
        self.assertFalse((home / 'data/runtime-upgrade-stop').exists())

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
                       "ComputeEngine", "EmsParityCheck", "EmsClusterCoordinator", "memory_point_store_migrate"):
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
    output = io.StringIO()
    suite = (unittest.TestSuite(InstallIsolationTest(name) for name in options.test) if options.test else
             unittest.defaultTestLoader.loadTestsFromTestCase(InstallIsolationTest))
    result = unittest.TextTestRunner(stream=output, verbosity=2).run(suite)
    report = output.getvalue()
    with (EVIDENCE / "result.txt").open("w") as stream:
        stream.write(report)
        stream.flush()
        os.fsync(stream.fileno())
    if (EVIDENCE / "result.txt").read_text() != report or '\0' in report:
        raise RuntimeError('result receipt readback mismatch')
    print(report)
    sys.exit(0 if result.wasSuccessful() else 1)
