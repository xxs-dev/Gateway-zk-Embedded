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
import tempfile
import unittest
from unittest import mock

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

    def without_qt(self):
        f = self.f
        f.approval['nodeId'] = 'COMM202600998'
        f.approval['offlineLocal']['nodeId'] = f.approval['nodeId']
        f.approval['coreReadOnlyTest']['profile'] = 'MONITOR_ONLY'
        f.approval['coreReadOnlyTest']['preservedRuntimeSha256'] = {}
        f.approval['units'] = ['camera-service@.service', 'compute-engine@.service', 'dio-driver@.service',
            'dlt645-driver@.service', 'event-engine@.service', 'gateway-services.service',
            'local-display-qt@.service', 'local-display@.service', 'local-kiosk@.service',
            'modbus-rtu@.service', 'mqtt-driver@.service', 'mqtt-tls-tunnel@.service',
            'system-monitor@.service', f.g.MONITOR_UNIT]
        for unit in (f.g.QT_UNIT, 'qt-display-bridge.service'):
            f.path('/etc/systemd/system', unit).unlink()
        (f.home/'ky-ems/KY-EMS').unlink()
        f.approval['installedRuntimeSha256'].pop('ky-ems/KY-EMS')
        (f.config/'qt-display.env').unlink()
        write(f.config/'device_identity.json', json.dumps({'machineCode': f.approval['nodeId']}))
        for p in (f.config/'devices').glob('*.json'): p.unlink()
        for p in (f.config/'apps').glob('*.json'):
            doc = json.loads(p.read_text())
            doc['deviceConfigFiles'] = []
            doc['mqtt']['enabled'] = False
            if 'localDisplay' in doc: doc['localDisplay']['enabled'] = False
            write(p, json.dumps(doc))
        f.approval['segments'] = [s for s in f.approval['segments'] if 'readonly_rtu' not in s['source']]
        self.refresh_config()
        original = f.g.systemctl_value
        def value(unit, prop):
            if unit in (f.g.QT_UNIT, 'qt-display-bridge.service'):
                properties = {'LoadState': 'not-found', 'ActiveState': 'inactive', 'MainPID': '0',
                              'FragmentPath': '', 'DropInPaths': '', 'NeedDaemonReload': 'no'}
                properties.update(f.overrides.get(unit, {}))
                return properties[prop]
            return original(unit, prop)
        f.patch(f.g, 'systemctl_value', value)

    def test_monitor_only_absent_qt_preserves_original_unit_scope(self):
        self.without_qt()
        f = self.f
        original_units = list(f.approval['units'])
        self.assertEqual([f.g.MONITOR_UNIT], f.g.core_scope(f.home, f.approval))
        f.apply()
        f.activate([f.g.MONITOR_UNIT])
        self.assertEqual({'gateway-services.service', f.g.MONITOR_UNIT}, f.live)
        f.m.recover(f.args)
        self.assertFalse(f.live)
        self.assertEqual(original_units, f.approval['units'])

    def test_monitor_only_omitted_qt_runtime_or_env_refused(self):
        self.without_qt()
        f = self.f
        for relative in ('bin/KY-EMS', 'bin/LocalDisplayQtEms', 'ky-ems/KY-EMS', 'config/runtime/qt-display.env'):
            with self.subTest(path=relative):
                p = f.home/relative
                write(p, 'unapproved Qt installation')
                try:
                    with self.assertRaisesRegex(ValueError, 'omitted Qt installation exists'):
                        f.g.core_scope(f.home, f.approval)
                finally: p.unlink()

    def test_monitor_only_omitted_qt_unit_file_or_dangling_alias_refused(self):
        self.without_qt()
        f = self.f
        for unit in (f.g.QT_UNIT, 'qt-display-bridge.service'):
            for root in ('/etc/systemd/system', '/run/systemd/transient', '/usr/lib/systemd/system'):
                for kind in ('regular', 'dangling'):
                    with self.subTest(unit=unit, root=root, kind=kind):
                        p = f.path(root)/unit
                        p.parent.mkdir(parents=True, exist_ok=True)
                        if kind == 'regular': write(p, '[Service]\nExecStart=/unapproved\n')
                        else: p.symlink_to(f.root/'missing-Qt-unit')
                        try:
                            with self.assertRaisesRegex(ValueError, 'omitted Qt unit file exists'):
                                f.g.core_scope(f.home, f.approval)
                        finally: p.unlink()

    def test_monitor_only_omitted_qt_dropin_or_dangling_coverage_refused(self):
        self.without_qt()
        f = self.f
        for dirname in ('/etc/systemd/system/ky-ems.service.d', '/run/systemd/system/qt-display-bridge.service.d',
                        '/etc/systemd/system/service.d', '/usr/lib/systemd/system/ky-.service.d',
                        '/run/systemd/generator.early/qt-display-.service.d'):
            for kind in ('extra', 'dangling-directory', 'dangling-file'):
                with self.subTest(directory=dirname, kind=kind):
                    directory = f.path(dirname)
                    directory.parent.mkdir(parents=True, exist_ok=True)
                    if kind == 'dangling-directory': directory.symlink_to(f.root/'missing-Qt-dropins')
                    else:
                        directory.mkdir(exist_ok=True)
                        extra = directory/'99-unapproved.conf'
                        if kind == 'extra': write(extra, '[Service]\nExecStartPre=/unapproved\n')
                        else: extra.symlink_to(f.root/'missing-Qt-override')
                    try:
                        with self.assertRaisesRegex(ValueError, 'omitted Qt unit coverage exists'):
                            f.g.core_scope(f.home, f.approval)
                    finally:
                        if kind == 'dangling-directory': directory.unlink()
                        else:
                            extra.unlink()
                            directory.rmdir()

    def test_monitor_only_omitted_qt_effective_presence_or_query_error_refused(self):
        self.without_qt()
        f = self.f
        for unit in (f.g.QT_UNIT, 'qt-display-bridge.service'):
            for prop, value in (('LoadState', 'loaded'), ('LoadState', 'masked'), ('ActiveState', 'active'),
                                ('MainPID', '91'), ('FragmentPath', '/unapproved'),
                                ('DropInPaths', '/unapproved.conf'), ('NeedDaemonReload', 'yes')):
                with self.subTest(unit=unit, prop=prop):
                    f.overrides[unit] = {prop: value}
                    with self.assertRaisesRegex(ValueError, 'omitted Qt unit is not absent'):
                        f.g.core_scope(f.home, f.approval)
                    f.overrides.clear()
        with mock.patch.object(f.g, 'systemctl_value', side_effect=ValueError('systemctl query failed')):
            with self.assertRaisesRegex(ValueError, 'query failed'):
                f.g.core_scope(f.home, f.approval)

    def test_monitor_only_omitted_qt_local_display_and_partial_scope_refused(self):
        self.without_qt()
        f = self.f
        documents = f.m.configured(f.config)
        documents['apps/monitor-service.json']['localDisplay']['enabled'] = True
        with self.assertRaisesRegex(ValueError, 'requires disabled local display'):
            f.g.core_documents_safe(f.home, f.approval, documents)
        f.approval['units'].append(f.g.QT_UNIT)
        with self.assertRaisesRegex(ValueError, 'independent approval and reader scope'):
            f.g.core_scope(f.home, f.approval)

    def test_monitor_only_omitted_qt_late_appearance_blocks_guard_without_scope_change(self):
        self.without_qt()
        f = self.f
        original_units = list(f.approval['units'])
        f.apply()
        f.activate([f.g.MONITOR_UNIT])
        write(f.path('/run/systemd/system/qt-display-bridge.service'), '[Service]\nExecStart=/unapproved\n')
        with self.assertRaisesRegex(ValueError, 'omitted Qt unit file exists'):
            f.g.activated_unit(f.home, f.g.MONITOR_UNIT)
        self.assertEqual(original_units, f.approval['units'])

    def test_999_still_requires_both_qt_and_bridge_participants(self):
        f = self.f
        for omitted in ((f.g.QT_UNIT,), ('qt-display-bridge.service',), (f.g.QT_UNIT, 'qt-display-bridge.service')):
            with self.subTest(omitted=omitted):
                approval = copy.deepcopy(f.approval)
                approval['units'] = [u for u in approval['units'] if u not in omitted]
                with self.assertRaisesRegex(ValueError, 'independent approval and reader scope'):
                    f.g.core_scope(f.home, approval)

    def test_monitor_only_missing_reader_or_launcher_refused(self):
        self.without_qt()
        f = self.f
        for omitted in (f.g.MONITOR_UNIT, 'gateway-services.service'):
            with self.subTest(omitted=omitted):
                approval = copy.deepcopy(f.approval)
                approval['units'].remove(omitted)
                with self.assertRaisesRegex(ValueError, 'independent approval and reader scope'):
                    f.g.core_scope(f.home, approval)

    def pinned_custom_monitor(self):
        self.without_qt()
        f = self.f
        implicit = f.m.MONITOR_IMPLICIT_SHM
        explicit = 'gateway_point_store_COMM202600998_monitor'
        source = f.path('/dev/shm')/implicit
        source.rename(f.path('/dev/shm')/explicit)
        for segment in f.approval['segments']:
            if segment['source'] == implicit: segment['source'] = explicit
        for p in (f.config/'apps').glob('*.json'):
            doc = f.m.switch_names(json.loads(p.read_text()), {implicit: explicit})
            if p.name == 'monitor-service.json': doc['systemMonitor']['sharedMemoryName'] = explicit
            write(p, json.dumps(doc))
        write(f.m.MONITOR_DROPIN, f.m.MONITOR_ENV + explicit.encode() + b'\n')
        f.m.MONITOR_DROPIN.chmod(0o644)
        f.approval['systemMonitorShmDropinSha256'] = sha(f.m.MONITOR_DROPIN)
        self.refresh_config()

    def test_pinned_custom_monitor_apply_activate_recover_preserves_source_and_scope(self):
        self.pinned_custom_monitor()
        f = self.f
        units = list(f.approval['units'])
        original_dropin = f.m.MONITOR_DROPIN.read_bytes()
        identities = {s['source']: ((f.path('/dev/shm')/s['source']).stat().st_dev,
                                   (f.path('/dev/shm')/s['source']).stat().st_ino,
                                   sha(f.path('/dev/shm')/s['source'])) for s in f.approval['segments']}
        self.assertEqual([f.g.MONITOR_UNIT], f.g.core_scope(f.home, f.approval))
        state = f.apply()
        self.assertEqual('UPGRADED_STOPPED', state['phase'])
        self.assertEqual('gateway_point_store_COMM202600998_monitor', state['systemMonitorShmDropin']['source'])
        f.activate([f.g.MONITOR_UNIT])
        self.assertEqual({'gateway-services.service', f.g.MONITOR_UNIT}, f.live)
        f.m.recover(f.args)
        self.assertFalse(f.live)
        self.assertEqual(units, f.approval['units'])
        self.assertEqual(original_dropin, f.m.MONITOR_DROPIN.read_bytes())
        for name, raw in f.originals.items(): self.assertEqual(raw, (f.config/name).read_bytes())
        for name, identity in identities.items():
            p = f.path('/dev/shm')/name
            self.assertEqual(identity, (p.stat().st_dev, p.stat().st_ino, sha(p)))
        self.assertFalse((f.path('/dev/shm')/f.m.MONITOR_IMPLICIT_SHM).exists())

    def test_pinned_custom_monitor_wrong_pin_refused_before_mutation(self):
        self.pinned_custom_monitor()
        f = self.f
        f.approval['systemMonitorShmDropinSha256'] = '0'*64
        with self.assertRaisesRegex(ValueError, 'monitor SHM drop-in SHA256 mismatch'):
            f.apply()
        self.assertFalse(f.state_dir.exists())
        self.assertFalse((f.home/'data/runtime-upgrade-stop').exists())

    def test_pinned_custom_monitor_unapproved_source_refused_before_mutation(self):
        self.pinned_custom_monitor()
        f = self.f
        write(f.m.MONITOR_DROPIN, f.m.MONITOR_ENV + b'unapproved_monitor_source\n')
        f.approval['systemMonitorShmDropinSha256'] = sha(f.m.MONITOR_DROPIN)
        with self.assertRaisesRegex(ValueError, 'binding source must be an approved segment'):
            f.apply()
        self.assertFalse(f.state_dir.exists())

    def test_pinned_custom_monitor_uncovered_canonical_refused(self):
        self.pinned_custom_monitor()
        f = self.f
        write(f.path('/dev/shm')/f.m.MONITOR_IMPLICIT_SHM, struct.pack('<II', 0x4d505354, 10))
        with self.assertRaisesRegex(ValueError, 'uncovered implicit SystemMonitor SHM segment'):
            f.apply()
        self.assertFalse(f.state_dir.exists())

    def test_unpinned_custom_monitor_still_requires_canonical(self):
        self.pinned_custom_monitor()
        f = self.f
        del f.approval['systemMonitorShmDropinSha256']
        with self.assertRaisesRegex(ValueError, 'core must cover implicit monitor SHM'):
            f.apply()
        self.assertFalse(f.state_dir.exists())

    def test_pinned_custom_monitor_changed_backup_refuses_activation(self):
        self.pinned_custom_monitor()
        f = self.f
        f.apply()
        write(f.state_dir/'monitor-dropin-old', f.m.MONITOR_ENV + b'unapproved_old_source\n')
        with self.assertRaisesRegex(ValueError, 'monitor SHM binding'):
            f.activate([f.g.MONITOR_UNIT])
        self.assertFalse(f.live)

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


class CoreDeniedTemplateTest(unittest.TestCase):
    def setUp(self):
        self.g = load().guard
        temp = tempfile.TemporaryDirectory(prefix='core-template-fence-')
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        def mapped(*parts):
            path = Path(*parts)
            return self.root / str(path).lstrip('/') if path.is_absolute() else path
        self.path = mapped
        patcher = mock.patch.object(self.g, 'Path', mapped)
        patcher.start()
        self.addCleanup(patcher.stop)
        self.template = 'event-engine@.service'
        self.instance = 'event-engine@excluded.service'
        self.approval = {'gatewayHome': '/opt/modbus-gateway', 'transactionId': 'synthetic-template',
                         'units': [self.template, self.instance]}
        self.queries = []
        def value(unit, prop):
            if unit.endswith('@.service'):
                raise ValueError('systemctl show failed: ' + unit + ' ' + prop)
            self.queries.append((unit, prop))
            path, _ = self.g.denied_dropin(mapped(self.approval['gatewayHome']),
                                          self.approval['transactionId'], unit)
            return {'ActiveState': 'inactive', 'MainPID': '0', 'DropInPaths': str(path)}[prop]
        self.value = value
        patcher = mock.patch.object(self.g, 'systemctl_value', value)
        patcher.start()
        self.addCleanup(patcher.stop)

    def fences(self, inhibited):
        home = self.path(self.approval['gatewayHome'])
        for unit in self.approval['units']:
            path, body = self.g.denied_dropin(home, self.approval['transactionId'], unit)
            if inhibited:
                body = ('[Unit]\nConditionPathExists=!' + str(home / 'data/runtime-upgrade-stop') + '\n').encode()
            write(path, body)
            path.chmod(0o644)
        return self.g.denied_dropin(home, self.approval['transactionId'], self.template)[0]

    def test_bare_template_is_not_queried_but_instances_are(self):
        for inhibited in (True, False):
            with self.subTest(inhibited=inhibited):
                self.fences(inhibited)
                self.queries.clear()
                self.g.core_denied_units(self.approval, [], inhibited=inhibited)
                self.assertEqual({(self.instance, p) for p in ('ActiveState', 'MainPID', 'DropInPaths')},
                                 set(self.queries))

    def test_template_missing_fence_refused(self):
        for inhibited in (True, False):
            with self.subTest(inhibited=inhibited):
                self.fences(inhibited).unlink()
                with self.assertRaisesRegex(ValueError, 'single-link regular'):
                    self.g.core_denied_units(self.approval, [], inhibited=inhibited)

    def test_template_changed_fence_refused(self):
        for inhibited in (True, False):
            with self.subTest(inhibited=inhibited):
                write(self.fences(inhibited), '[Unit]\nConditionPathExists=/unapproved\n')
                with self.assertRaisesRegex(ValueError, 'fence coverage'):
                    self.g.core_denied_units(self.approval, [], inhibited=inhibited)

    def test_template_wrong_mode_refused(self):
        for inhibited in (True, False):
            for mode in (0o600, 0o666, 0o755):
                with self.subTest(inhibited=inhibited, mode=mode):
                    self.fences(inhibited).chmod(mode)
                    with self.assertRaisesRegex(ValueError, 'fence coverage'):
                        self.g.core_denied_units(self.approval, [], inhibited=inhibited)

    def test_template_extra_dropins_refused_in_fixed_search_directories(self):
        for inhibited in (True, False):
            for dirname in ('etc/systemd/system/' + self.template + '.d',
                            'run/systemd/system/' + self.template + '.d',
                            'usr/lib/systemd/system/' + self.template + '.d',
                            'run/systemd/generator.early/' + self.template + '.d',
                            'etc/systemd/system/service.d',
                            'etc/systemd/system/event-.service.d'):
                with self.subTest(inhibited=inhibited, directory=dirname):
                    self.fences(inhibited)
                    extra = self.root / dirname / '99-unapproved.conf'
                    write(extra, '[Service]\nExecStartPre=/unapproved\n')
                    try:
                        with self.assertRaisesRegex(ValueError, 'fence coverage'):
                            self.g.core_denied_units(self.approval, [], inhibited=inhibited)
                    finally:
                        extra.unlink()

    def test_template_nonregular_or_multilink_fence_refused(self):
        for kind in ('symlink', 'hardlink', 'directory'):
            with self.subTest(kind=kind):
                p = self.fences(True)
                original = p.read_bytes()
                p.unlink()
                target = self.root / 'unapproved-fence'
                write(target, original)
                if kind == 'symlink': p.symlink_to(target)
                elif kind == 'hardlink': os.link(target, p)
                else: p.mkdir()
                try:
                    with self.assertRaisesRegex(ValueError, 'single-link regular'):
                        self.g.core_denied_units(self.approval, [], inhibited=True)
                finally:
                    if kind == 'directory': p.rmdir()
                    else: p.unlink()
                    target.unlink()

    def test_instance_effective_coverage_and_stopped_gates_remain_strict(self):
        for prop, value, error in (('DropInPaths', '/unapproved.conf', 'fence coverage'),
                                   ('ActiveState', 'active', 'not stopped'),
                                   ('MainPID', '91', 'not stopped')):
            with self.subTest(property=prop):
                self.fences(True)
                def changed(unit, requested):
                    return value if unit == self.instance and requested == prop else self.value(unit, requested)
                with mock.patch.object(self.g, 'systemctl_value', changed):
                    with self.assertRaisesRegex(ValueError, error):
                        self.g.core_denied_units(self.approval, [], inhibited=True)


if __name__ == '__main__':
    unittest.main()
