"""Local file transactions only; no device, broker, real daemon or migration.

Reuse the existing systemctl/migration fixture. Pins are synthetic approved
manifest data, never substitutes for the retained historical profile constants.
"""
import json
import copy
import os
from pathlib import Path
import struct
import subprocess
import unittest

import a_acquisition_activation_test as fixture

load, sha, write = fixture.load, fixture.sha, fixture.write

PROGRAMS = ('ModbusRtu Dlt645Driver DioDriver CanDriver IecDriver MqttDriver '
            'MqttForwarder EventEngine ComputeEngine EmsClusterCoordinator '
            'AgcAvcController EmsParityCheck SystemMonitor LocalDisplay '
            'QtDisplayBridge CameraService pointctl stress_runner memory_point_store_migrate').split()
SCRIPTS = ('runtime-upgrade-guard.py', 'gateway-services.sh', 'ota-apply.sh',
           'ota-rollback.sh', 'ota-health-result.py', 'production-smoke-test.sh')


class CoreReadOnlyTest(unittest.TestCase):
    def setUp(self):
        original = load()
        f = self.f = fixture.AcquisitionTest('test_apply_activate_recover_restores_exact_bytes')
        f.setUp()
        self.addCleanup(f.doCleanups)
        for owner, key, value in ((f.g, 'MONITOR_BINARY', original.guard.MONITOR_BINARY),
                                   (f.g, 'QT_BINARY', original.guard.QT_BINARY),
                                   (f.m, 'B_MONITOR_BINARY_SHA256', original.B_MONITOR_BINARY_SHA256),
                                   (f.m, 'A_QT_BINARY_SHA256', original.A_QT_BINARY_SHA256)):
            setattr(owner, key, value)
        f.g.A_SOURCE_CONFIGS = original.guard.A_SOURCE_CONFIGS.copy()
        for key in ('aReadonlyAcquisition', 'qtDisplayEnvSha256', 'aInboundDisableSourceSha256', 'scadaReadOnlyProject'):
            f.approval.pop(key, None)
        f.approval['nodeId'] = 'COMM202600999'
        f.approval['offlineLocal']['nodeId'] = f.approval['nodeId']
        write(f.config/'device_identity.json', json.dumps({'machineCode': f.approval['nodeId']}))
        components = []
        for name in PROGRAMS:
            p = f.root/'payload/bin'/name
            header = bytearray(64)
            header[:6] = b'\x7fELF\x02\x01'
            struct.pack_into('<HH', header, 16, 2, 183)
            write(p, bytes(header) + ('synthetic approved ' + name).encode())
            components.append({'kind': 'product', 'target': name, 'archivePath': 'bin/'+name,
                               'sha256': sha(p), 'bytes': p.stat().st_size})
        manifest = {'sourceCommit': 'cb45f3fc78cd6288d9bb49f06f93ca07b441ecfa', 'components': components}
        write(f.root/'manifest.json', json.dumps(manifest))
        f.approval['programManifestSha256'] = sha(f.root/'manifest.json')
        f.approval['installPaths'] = {c['target']: 'bin/'+c['target'] for c in components}
        f.approval['coreReadOnlyTest'] = {
            'schemaVersion': 'core-readonly-test-1', 'profile': 'RTU_MQTT_MONITOR',
            'sourceCommit': manifest['sourceCommit'], 'programManifestSha256': sha(f.root/'manifest.json'),
            'preservedRuntimeSha256': {'ky-ems/KY-EMS': sha(f.home/'ky-ems/KY-EMS')}}
        for name in SCRIPTS[2:]:
            write(f.home/'bin'/name, 'old script '+name)
            (f.home/'bin'/name).chmod(0o755)
            f.approval['activationStartupScripts'][name] = {
                'oldSha256': sha(f.home/'bin'/name),
                'newSha256': sha(Path(__file__).resolve().parent.parent/'deploy'/name), 'mode': 0o755}
        for segment in f.approval['segments']:
            p = f.path('/dev/shm')/segment['source']
            with p.open('r+b') as out: out.truncate(15174272)
            segment['sha256'] = sha(p)
        self.refresh_config()
        self.initial_sources = {s['source']: (f.path('/dev/shm')/s['source']).stat().st_ino for s in f.approval['segments']}
        old_external = f.external
        def external(command, **kwargs):
            if '--copy-to-v11' in command:
                source = command[command.index('--shm')+1]
                target = command[command.index('--copy-to-v11')+1]
                write(Path(command[command.index('--backup')+1]), (f.path('/dev/shm')/source).read_bytes())
                p = f.path('/dev/shm')/target
                write(p, struct.pack('<II', 0x4d505354, 11))
                with p.open('r+b') as out: out.truncate(15667032)
                return subprocess.CompletedProcess(command, 0, b'simulated migration; not real OTA\n')
            return old_external(command, **kwargs)
        f.patch(f.m.subprocess, 'run', external)

    def refresh_config(self):
        f = self.f
        f.approval['identitySha256'] = sha(f.config/'device_identity.json')
        f.approval['configSha256'] = f.m.tree_hashes(f.config)
        f.originals = {name: (f.config/name).read_bytes() for name in f.approval['configSha256']}

    def readers(self):
        f = self.f
        return [f.g.A_RTU_UNIT, f.g.A_MQTT_UNIT, f.g.MONITOR_UNIT]

    def test_current_manifest_three_readers_apply_activate_recover(self):
        f = self.f
        old_qt = (f.home/'ky-ems/KY-EMS').read_bytes()
        state = f.apply()
        self.assertEqual('UPGRADED_STOPPED', state['phase'])
        f.activate(self.readers())
        self.assertEqual(set(self.readers()+['gateway-services.service']), f.live)
        self.assertEqual(old_qt, (f.home/'ky-ems/KY-EMS').read_bytes())
        for s in f.approval['segments']:
            p = f.path('/dev/shm')/s['source']
            self.assertEqual(s['sha256'], sha(p))
            self.assertEqual(self.initial_sources[s['source']], p.stat().st_ino)
        f.m.recover(f.args)
        self.assertFalse(f.live)
        self.assertFalse(f.m.MONITOR_DROPIN.exists())
        for name, raw in f.originals.items(): self.assertEqual(raw, (f.config/name).read_bytes())
        for unit in f.approval['units']:
            path, body = f.m.inhibition_dropin(f.home, f.approval['transactionId'], unit)
            self.assertEqual(body, path.read_bytes())

    def test_monitor_only_has_no_devices_or_mqtt_and_no_watchdog_start(self):
        f = self.f
        f.approval['nodeId'] = 'COMM202600998'
        f.approval['offlineLocal']['nodeId'] = f.approval['nodeId']
        f.approval['coreReadOnlyTest']['profile'] = 'MONITOR_ONLY'
        f.approval['units'].append('gateway-health-watchdog.service')
        write(f.config/'device_identity.json', json.dumps({'machineCode': f.approval['nodeId']}))
        for p in (f.config/'devices').glob('*.json'): p.unlink()
        for p in (f.config/'apps').glob('*.json'):
            doc = json.loads(p.read_text())
            doc['deviceConfigFiles'] = []
            doc['mqtt']['enabled'] = False
            write(p, json.dumps(doc))
        f.approval['segments'] = [s for s in f.approval['segments'] if 'readonly_rtu' not in s['source']]
        self.refresh_config()
        f.apply()
        f.activate([f.g.MONITOR_UNIT])
        self.assertEqual({'gateway-services.service', f.g.MONITOR_UNIT}, f.live)
        f.m.recover(f.args)
        self.assertFalse(f.live)

    def test_core_cannot_add_qt_or_control_unit_to_ready(self):
        f = self.f
        f.apply()
        for readers in (self.readers()+[f.g.QT_UNIT], [f.g.MONITOR_UNIT],
                        self.readers()+['agc-avc@control.service']):
            with self.assertRaisesRegex(ValueError, 'exact approved readers'):
                f.activate(readers)
            self.assertFalse(f.live)

    def test_unapproved_elf_target_refused_even_if_manifest_rehashed(self):
        f = self.f
        manifest = json.loads((f.root/'manifest.json').read_text())
        manifest['components'][0]['target'] = 'arbitrary_control'
        write(f.root/'manifest.json', json.dumps(manifest))
        f.approval['programManifestSha256'] = sha(f.root/'manifest.json')
        f.approval['coreReadOnlyTest']['programManifestSha256'] = f.approval['programManifestSha256']
        with self.assertRaisesRegex(ValueError, 'complete exact approved'):
            f.apply()
        self.assertFalse(f.state_dir.exists())

    def test_manifest_pin_and_preserved_qt_inventory_fail_closed(self):
        f = self.f
        old = f.approval['coreReadOnlyTest']['programManifestSha256']
        f.approval['coreReadOnlyTest']['programManifestSha256'] = '0'*64
        with self.assertRaisesRegex(ValueError, 'independent approval'):
            f.apply()
        f.approval['coreReadOnlyTest']['programManifestSha256'] = old
        write(f.home/'ky-ems/KY-EMS', 'changed unrelated Qt')
        with self.assertRaisesRegex(ValueError, 'preserved runtime'):
            f.apply()
        self.assertFalse(f.state_dir.exists())

    def test_layout_size_mismatch_refused_before_fence_or_replacement(self):
        f = self.f
        p = f.path('/dev/shm')/f.approval['segments'][0]['source']
        with p.open('r+b') as out: out.truncate(15174271)
        f.approval['segments'][0]['sha256'] = sha(p)
        old = (f.home/'bin/ModbusRtu').read_bytes()
        with self.assertRaisesRegex(ValueError, 'layout/version/size'):
            f.apply()
        self.assertFalse(f.state_dir.exists())
        self.assertFalse((f.home/'data/runtime-upgrade-stop').exists())
        self.assertEqual(old, (f.home/'bin/ModbusRtu').read_bytes())

    def test_same_bytes_new_source_inode_rejected_before_replace(self):
        f = self.f
        original = f.m.subprocess.run
        p = f.path('/dev/shm')/f.approval['segments'][0]['source']
        def external(command, **kwargs):
            result = original(command, **kwargs)
            if '--copy-to-v11' in command:
                twin = p.with_name('same_bytes_unapproved_inode')
                write(twin, p.read_bytes())
                os.replace(twin, p)
            return result
        f.patch(f.m.subprocess, 'run', external)
        old = (f.home/'bin/ModbusRtu').read_bytes()
        with self.assertRaisesRegex(ValueError, 'source SHM identity'):
            f.apply()
        self.assertEqual(old, (f.home/'bin/ModbusRtu').read_bytes())

    def test_absent_monitor_dangling_symlink_recovery_is_not_success(self):
        f = self.f
        f.apply()
        f.m.MONITOR_DROPIN.unlink()
        f.m.MONITOR_DROPIN.symlink_to(f.root/'missing-target')
        original = f.systemctl
        # systemctl metadata does not open the broken file; the file gate must reject it.
        def systemctl(*args):
            if args[0]=='show' and args[-1]==f.g.MONITOR_UNIT and args[1]=='--property=ActiveState':
                return 'inactive'
            return original(*args)
        f.patch(f.m, 'systemctl', systemctl)
        with self.assertRaisesRegex(ValueError, 'symlink'):
            f.m.recover(f.args)
        self.assertTrue(f.m.MONITOR_DROPIN.is_symlink())
        self.assertNotEqual('RECOVERED_STOPPED', json.loads((f.state_dir/'state.json').read_text())['phase'])

    def test_absent_old_monitor_backup_dangling_symlink_refused(self):
        f = self.f
        f.apply()
        (f.state_dir/'monitor-dropin-old').symlink_to(f.root/'missing-old')
        with self.assertRaisesRegex(ValueError, 'absent core monitor'):
            f.m.recover(f.args)

    def test_writable_points_are_refused_even_when_config_reapproved(self):
        f = self.f
        p = f.config/'devices/device_modbusRTU_2_readonly.json'
        doc = json.loads(p.read_text())
        doc['meters'][0]['points'][0]['write']['enable'] = True
        write(p, json.dumps(doc))
        self.refresh_config()
        with self.assertRaisesRegex(ValueError, 'writable points'):
            f.apply()
        self.assertFalse(f.state_dir.exists())

    def test_rehashed_state_cannot_expand_configuration_patch(self):
        f = self.f
        state = f.apply()
        p = f.config/'apps/mqtt-service.json'
        doc = json.loads(p.read_text())
        doc['mqtt']['username'] = 'unapproved change'
        write(p, json.dumps(doc))
        state['configSha256']['apps/mqtt-service.json'] = sha(p)
        next(row for row in state['files'] if row['path']=='config/runtime/apps/mqtt-service.json')['newSha256'] = sha(p)
        with self.assertRaisesRegex(ValueError, 'exceeds internal SHM remap'):
            f.g.core_config(f.home, f.state_dir, f.approval, state)

    def test_start_failure_refences_and_restores_without_source_mutation(self):
        f = self.f
        f.apply()
        f.fail_start = f.g.A_MQTT_UNIT
        with self.assertRaisesRegex(ValueError, 'start failure'):
            f.activate(self.readers())
        self.assertFalse(f.live)
        f.m.recover(f.args)
        for s in f.approval['segments']:
            self.assertEqual(s['sha256'], sha(f.path('/dev/shm')/s['source']))
        for name, raw in f.originals.items(): self.assertEqual(raw, (f.config/name).read_bytes())

    def historical_predecessor(self):
        f = self.f
        core_approval = copy.deepcopy(f.approval)
        core_manifest = (f.root/'manifest.json').read_bytes()
        legacy = copy.deepcopy(core_approval)
        del legacy['coreReadOnlyTest']
        legacy.update(aReadonlyAcquisition=True, qtDisplayEnvSha256=sha(f.config/'qt-display.env'),
                      aInboundDisableSourceSha256=sha(f.config/'apps/monitor-service.json'),
                      scadaReadOnlyProject={'oldTarget':'releases/old','newTarget':'releases/new'})
        legacy['activationStartupScripts'] = {k:v for k,v in legacy['activationStartupScripts'].items() if k in f.m.ACTIVATION_SCRIPTS}
        manifest = json.loads(core_manifest)
        qt = f.root/'payload/ky-ems/KY-EMS'
        manifest['components'].append({'kind':'product','target':'LocalDisplayQtEms',
                                       'archivePath':'ky-ems/KY-EMS','sha256':sha(qt),'bytes':qt.stat().st_size})
        legacy['installPaths']['LocalDisplayQtEms'] = 'ky-ems/KY-EMS'
        write(f.root/'manifest.json', json.dumps(manifest))
        legacy['programManifestSha256'] = sha(f.root/'manifest.json')
        real_constants = (f.g.MONITOR_BINARY, f.g.QT_BINARY, f.m.B_MONITOR_BINARY_SHA256, f.m.A_QT_BINARY_SHA256)
        monitor = next(c['sha256'] for c in manifest['components'] if c['target']=='SystemMonitor')
        # Only the legacy synthetic fixture substitutes its historical pins.
        f.g.MONITOR_BINARY = f.m.B_MONITOR_BINARY_SHA256 = monitor
        f.g.QT_BINARY = f.m.A_QT_BINARY_SHA256 = sha(qt)
        f.g.A_SOURCE_CONFIGS = {name:sha(f.config/name) for name in f.documents}
        f.approval = legacy
        f.apply()
        f.activate(f.g.A_ACQUISITION_READERS)
        f.m.recover(f.args)
        old_dir, old_sha = f.state_dir, f.args.approval_sha256
        recovered = json.loads((old_dir/'state.json').read_text())
        receipt = {'schemaVersion':'offline-shm11-recovered-1','phase':'RECOVERED_STOPPED',
                   'controlEnabled':False,'transactionId':legacy['transactionId'],'nodeId':legacy['nodeId'],
                   'gatewayHome':str(f.home),'approvalSha256':old_sha,'stateSha256':sha(old_dir/'state.json'),
                   'retainedTargetsSha256':{s['target']:sha(f.path('/dev/shm')/s['target']) for s in legacy['segments']}}
        receipt_path = f.root/'recovered-receipt.json'
        write(receipt_path, json.dumps(receipt))
        f.g.MONITOR_BINARY, f.g.QT_BINARY, f.m.B_MONITOR_BINARY_SHA256, f.m.A_QT_BINARY_SHA256 = real_constants
        f.approval = core_approval
        f.approval['transactionId'] = 'synthetic-core-successor'
        for key in ('qtDisplayEnvSha256','aInboundDisableSourceSha256','scadaReadOnlyProject'):
            f.approval[key] = legacy[key]
        for i,s in enumerate(f.approval['segments']): s['target'] = 'core_successor_target_'+str(i)
        f.approval['recoveredFrom'] = {'stateDir':str(old_dir),'stateSha256':sha(old_dir/'state.json'),
                                      'approvalSha256':old_sha,'receiptPath':str(receipt_path),
                                      'receiptSha256':sha(receipt_path)}
        f.state_dir = f.root/'core-successor-state'
        write(f.root/'manifest.json', core_manifest)
        return old_dir, recovered

    def test_historical_metadata_is_preserved_not_an_a_profile_patch(self):
        f = self.f
        self.historical_predecessor()
        qt_env = (f.config/'qt-display.env').read_bytes()
        monitor_before = json.loads((f.config/'apps/monitor-service.json').read_text())
        f.apply()
        f.activate(self.readers())
        monitor_after = json.loads((f.config/'apps/monitor-service.json').read_text())
        for key in ('mqtt','systemMonitor'):
            self.assertEqual(monitor_before[key], monitor_after[key])
        self.assertEqual(qt_env, (f.config/'qt-display.env').read_bytes())
        self.assertNotIn(f.g.QT_UNIT, f.live)
        self.assertEqual('releases/old', os.readlink(f.home/'scada/current'))
        f.m.recover(f.args)
        for name,raw in f.originals.items():self.assertEqual(raw, (f.config/name).read_bytes())

    def test_historical_metadata_or_receipt_drift_still_blocks_successor(self):
        f = self.f
        self.historical_predecessor()
        f.approval['qtDisplayEnvSha256'] = '0'*64
        with self.assertRaisesRegex(ValueError, 'inherited metadata'):
            f.apply()
        f.approval['qtDisplayEnvSha256'] = f.approval['configSha256']['qt-display.env']
        f.approval['recoveredFrom']['stateSha256'] = '0'*64
        with self.assertRaisesRegex(ValueError, 'SHA256 mismatch'):
            f.apply()
        self.assertFalse(f.state_dir.exists())

    def test_open_source_fd_refused_even_without_mapping(self):
        f = self.f
        pid = f.path('/proc/900001')
        write(pid/'maps', '')
        (pid/'fd').mkdir()
        (pid/'fd/5').symlink_to(f.path('/dev/shm')/f.approval['segments'][0]['source'])
        with self.assertRaisesRegex(ValueError, 'SHM still open'):
            f.apply()
        self.assertFalse(f.state_dir.exists())

    def test_source_identity_rechecked_between_apply_and_activation(self):
        f = self.f
        f.apply()
        p = f.path('/dev/shm')/f.approval['segments'][0]['source']
        twin = p.with_name('unapproved_identical_clone')
        write(twin, p.read_bytes())
        os.replace(twin, p)
        with self.assertRaisesRegex(ValueError, 'source SHM identity'):
            f.activate(self.readers())
        self.assertFalse(f.live)

    def test_late_excluded_participant_start_or_override_refused(self):
        f = self.f
        f.apply()
        f.activate(self.readers())
        f.live.add(f.g.QT_UNIT)
        with self.assertRaisesRegex(ValueError, 'excluded participant is not stopped'):
            f.g.activated_unit(f.home, f.g.A_RTU_UNIT)
        f.live.remove(f.g.QT_UNIT)
        f.overrides[f.g.QT_UNIT] = {'DropInPaths': 'unapproved-later-override.conf'}
        with self.assertRaisesRegex(ValueError, 'fence coverage'):
            f.g.activated_unit(f.home, f.g.A_RTU_UNIT)

    def test_ordinary_ota_still_blocked_by_offline_fence(self):
        f = self.f
        f.apply()
        f.activate(self.readers())
        manifest = f.root/'script-ota-manifest.json'
        write(manifest, json.dumps({'files': []}))
        with self.assertRaisesRegex(ValueError, 'offline upgrade fence active'):
            f.g.ota(manifest)


if __name__ == '__main__':
    unittest.main()
