#!/usr/bin/env python3
"""Private filesystem fixtures; no host systemd action or program execution."""
import importlib.util
import json
import os
import signal
from pathlib import Path
import tempfile
import time
import types
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[1]


@unittest.skipUnless(getattr(os, 'geteuid', lambda: -1)() == 0, 'root-owned private filesystem fixture')
class ReprovisionTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='gateway-reprovision-fixture-', dir='/opt')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        spec = importlib.util.spec_from_file_location('reprovision_fixture', str(REPO / 'deploy/gateway-reprovision.py'))
        self.m = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.m)
        self.home = self.root / 'home'
        self.image = self.root / 'image'
        self.units = self.root / 'systemd'
        self.new_units = self.root / 'new-systemd'
        self.journal = self.root / 'journal'
        for name, value in (('HOME', self.home), ('ROOT', self.journal), ('UNIT_DIR', self.units)):
            self.patch(self.m, name, value)
        self.patch(self.m.os, 'uname', lambda: types.SimpleNamespace(machine='aarch64'))
        self.patch(self.m.guard, 'SYSTEMD_ROOTS', (str(self.units),))
        self.original_units = {'gateway-services.service', 'system-monitor@.service',
                               'system-monitor@monitor-service.service', 'gateway-ota-recover.service'}
        self.present_units = self.m.present_units
        self.patch(self.m, 'present_units', lambda: set(self.original_units))
        self.patch(self.m, 'processes', lambda: [])
        self.calls = []
        self.fail_stop = False
        self.patch(self.m.lifecycle, 'systemctl', self.systemctl)
        for home, revision in ((self.home, b'old'), (self.image, b'new')):
            for name in ('bin', 'config/runtime', 'ky-ems', 'data', 'run'):
                (home / name).mkdir(parents=True, mode=0o755)
            self.write(home / 'config/runtime/device_identity.json', b'{"machineCode":"FIXTURE"}\n')
            self.write(home / 'bin/gateway-services.sh', revision + b' launcher\n', 0o755)
            self.write(home / 'bin/runtime-upgrade-guard.py', revision + b' guard\n', 0o755)
        self.write(self.home / 'data/retained-ledger.json', b'{"receipt":"do-not-replay"}\n', 0o600)
        self.write(self.home / 'run/diagnostic.log', b'original mutable history\n', 0o600)
        for name in ('gateway-services.service', 'system-monitor@.service', 'gateway-ota-recover.service'):
            self.write(self.units / name, b'[Service]\nExecStart=/fixture/old\n')
        self.write(self.units / 'system-monitor@monitor-service.service.d/90-offline-original.conf',
                   b'[Unit]\nConditionPathExists=/fixture/old-proof\n')
        for name in ('gateway-services.service', 'ky-ems.service', 'gateway-ota-recover.service'):
            self.write(self.new_units / name, b'[Service]\nExecStart=/fixture/new\n')
        self.runtime = self.m.Reprovision()
        self.m.root_setup()
        self.old_tree = self.m.tree(self.home)
        self.old_id = self.m.identity(self.home)

    def patch(self, *args, **kwargs):
        p = mock.patch.object(*args, **kwargs)
        value = p.start()
        self.addCleanup(p.stop)
        return value

    def write(self, path, raw, mode=0o644):
        path.parent.mkdir(parents=True, exist_ok=True, mode=0o755)
        path.write_bytes(raw)
        path.chmod(mode)

    def systemctl(self, *args):
        self.calls.append(args)
        if args[0] == 'daemon-reload':
            return ''
        if args[0] == 'stop':
            if self.fail_stop:
                raise ValueError('injected stop refusal')
            return ''
        if args[:3] == ('show', '--property=LoadState', '--value'):
            return 'loaded' if args[3] in self.original_units or (self.units / args[3]).exists() else 'not-found'
        if args[:3] == ('show', '--property=ActiveState', '--value'):
            return 'inactive'
        if args[:3] == ('show', '--property=MainPID', '--value'):
            return '0'
        if args[:3] == ('show', '--property=NeedDaemonReload', '--value'):
            return 'no'
        if args[:3] == ('show', '--property=DropInPaths', '--value'):
            return str(self.units / (args[3] + '.d') / self.m.FENCE_NAME)
        raise AssertionError('unexpected systemd operation: ' + repr(args))

    def approval(self, name='TX_ONE'):
        value = self.m.plan(self.image, self.new_units, name, int(time.time()) + 600)
        path = self.root / (name + '.json')
        self.m.record(path, value, new=True)
        return path, self.m.digest(path)

    def install(self, name='TX_ONE'):
        path, pin = self.approval(name)
        transaction = self.runtime.apply(path, pin)
        return transaction, pin

    def assert_stopped_only(self):
        self.assertEqual((self.journal / 'STOP').read_bytes(), self.m.STOP_BODY)
        self.assertFalse(any(call[0] in ('start', 'restart', 'enable', 'reset-failed') for call in self.calls))
        self.assertTrue(any(call[0] == 'stop' for call in self.calls))

    def test_install_expands_units_and_keeps_original_image(self):
        transaction, _ = self.install()
        self.assertEqual(self.m.tree(self.home), self.m.tree(self.image))
        self.assertEqual(self.m.tree(transaction / 'previous-home'), self.old_tree)
        self.assertEqual(self.m.identity(transaction / 'previous-home'), self.old_id)
        self.assertTrue((self.units / 'ky-ems.service').exists())
        self.assertFalse((self.units / 'system-monitor@.service').exists())
        self.assertFalse((self.journal / 'pending.json').exists())
        self.assertEqual(self.m.read_record(transaction / 'state.json')['phase'], 'INSTALLED_STOPPED')
        self.assert_stopped_only()

    def test_recover_is_idempotent_and_does_not_replay_runtime_state(self):
        transaction, pin = self.install()
        self.write(self.home / 'data/new-history.json', b'new runtime history\n', 0o600)
        self.runtime.recover('TX_ONE', pin)
        self.runtime.recover('TX_ONE', pin)
        self.assertEqual(self.m.tree(self.home), self.old_tree)
        self.assertEqual(self.m.identity(self.home), self.old_id)
        self.assertEqual((transaction / 'retired-candidate/data/new-history.json').read_bytes(), b'new runtime history\n')
        self.assertFalse((self.units / 'ky-ems.service').exists())
        self.assertTrue((self.units / 'system-monitor@.service').exists())
        self.assertFalse((self.journal / 'current.json').exists())
        self.assert_stopped_only()

    def test_source_drift_refused_before_fence(self):
        path, pin = self.approval()
        self.write(self.image / 'bin/gateway-services.sh', b'changed after approval', 0o755)
        with self.assertRaises(ValueError):
            self.runtime.apply(path, pin)
        self.assertEqual(self.m.tree(self.home), self.old_tree)
        self.assertFalse((self.journal / 'STOP').exists())
        self.assertFalse(self.calls)

    def test_stop_failure_leaves_original_and_persistent_fence(self):
        path, pin = self.approval()
        self.fail_stop = True
        with self.assertRaisesRegex(ValueError, 'stop refusal'):
            self.runtime.apply(path, pin)
        self.assertEqual(self.m.tree(self.home), self.old_tree)
        self.assertTrue((self.journal / 'pending.json').exists())
        self.fail_stop = False
        self.runtime.recover('TX_ONE', pin)
        self.assertEqual(self.m.tree(self.home), self.old_tree)
        self.assert_stopped_only()

    def test_residual_process_blocks_switch(self):
        path, pin = self.approval()
        with mock.patch.object(self.m, 'processes', return_value=['123']):
            with self.assertRaisesRegex(ValueError, 'participants remain'):
                self.runtime.apply(path, pin)
        self.assertEqual(self.m.tree(self.home), self.old_tree)
        self.runtime.recover('TX_ONE', pin)
        self.assert_stopped_only()

    def test_effective_fence_missing_blocks_switch(self):
        path, pin = self.approval()
        def missing(*args):
            if args[:3] == ('show', '--property=DropInPaths', '--value'):
                return ''
            return self.systemctl(*args)
        with mock.patch.object(self.m.lifecycle, 'systemctl', side_effect=missing):
            with self.assertRaisesRegex(ValueError, 'has not loaded'):
                self.runtime.apply(path, pin)
        self.assertEqual(self.m.tree(self.home), self.old_tree)
        self.runtime.recover('TX_ONE', pin)

    def test_low_disk_space_refused_before_staging_or_stop(self):
        path, pin = self.approval()
        with mock.patch.object(self.m.os, 'statvfs', return_value=types.SimpleNamespace(f_bavail=1, f_frsize=4096)):
            with self.assertRaisesRegex(ValueError, 'durable staging space'):
                self.runtime.apply(path, pin)
        self.assertFalse((self.journal / 'transactions/TX_ONE').exists())
        self.assertFalse((self.journal / 'STOP').exists())

    def test_separate_bind_mount_refused_before_stop(self):
        path, pin = self.approval()
        with mock.patch.object(self.m, 'mount_id', side_effect=lambda p: 2 if p == self.units else 1):
            with self.assertRaisesRegex(ValueError, 'one mount'):
                self.runtime.apply(path, pin)
        self.assertFalse((self.journal / 'transactions/TX_ONE').exists())
        self.assertFalse((self.journal / 'STOP').exists())
        self.assertEqual(self.m.tree(self.home), self.old_tree)

    def test_wrong_approval_pin_has_no_live_side_effect(self):
        path, _ = self.approval()
        with self.assertRaisesRegex(ValueError, 'digest mismatch'):
            self.runtime.apply(path, '0' * 64)
        self.assertEqual(self.m.tree(self.home), self.old_tree)
        self.assertFalse(self.calls)

    def test_mutable_history_is_captured_after_stop_not_pinned_while_running(self):
        path, pin = self.approval()
        self.write(self.home / 'run/diagnostic.log', b'newer before stop\n', 0o600)
        transaction = self.runtime.apply(path, pin)
        self.assertEqual((transaction / 'previous-home/run/diagnostic.log').read_bytes(), b'newer before stop\n')
        self.runtime.recover('TX_ONE', pin)
        self.assertEqual((self.home / 'run/diagnostic.log').read_bytes(), b'newer before stop\n')

    def test_expired_plan_is_rejected_before_stop(self):
        path, pin = self.approval()
        with mock.patch.object(self.m.time, 'time', return_value=time.time() + 1000):
            with self.assertRaisesRegex(ValueError, 'expiry'):
                self.runtime.apply(path, pin)
        self.assertFalse((self.journal / 'STOP').exists())

    def test_unit_backup_journal_drift_is_rejected_before_recovery(self):
        transaction, pin = self.install()
        state = self.m.read_record(transaction / 'state.json')
        state['oldUnits'][str(self.units / 'gateway-services.service')] = '../foreign'
        self.m.record(transaction / 'state.json', state)
        calls = len(self.calls)
        with self.assertRaisesRegex(ValueError, 'backup journal changed'):
            self.runtime.recover('TX_ONE', pin)
        self.assertEqual(len(self.calls), calls)

    def test_inactive_instances_discovered_from_override_directories(self):
        with mock.patch.object(self.m.lifecycle, 'systemctl', return_value='gateway-services.service disabled'):
            units = self.present_units()
        self.assertIn('system-monitor@monitor-service.service', units)
        self.assertIn('system-monitor@.service', units)

    def test_shared_service_override_is_preserved_not_retired(self):
        path = self.units / 'service.d/10-resource-policy.conf'
        self.write(path, b'[Service]\nTasksMax=64\n')
        transaction, pin = self.install()
        self.assertEqual(path.read_bytes(), b'[Service]\nTasksMax=64\n')
        self.runtime.recover('TX_ONE', pin)
        self.assertEqual(path.read_bytes(), b'[Service]\nTasksMax=64\n')

    def interrupted(self, phase):
        path, pin = self.approval()
        original = self.runtime.checkpoint
        def interrupted(transaction, state, reached):
            original(transaction, state, reached)
            if reached == phase:
                raise RuntimeError('simulated process loss')
        with mock.patch.object(self.runtime, 'checkpoint', side_effect=interrupted):
            with self.assertRaisesRegex(RuntimeError, 'process loss'):
                self.runtime.apply(path, pin)
        self.runtime.recover('TX_ONE', pin)
        self.assertEqual(self.m.tree(self.home), self.old_tree)
        self.assert_stopped_only()

    def test_recovery_after_switch_intent(self):
        self.interrupted('SWITCH_INTENT')

    def test_recovery_after_old_directory_move(self):
        self.interrupted('OLD_MOVED')

    def test_recovery_after_new_directory_move(self):
        self.interrupted('IMAGE_INSTALLED')

    def test_recovery_before_current_pointer_publication(self):
        self.interrupted('INSTALLED_STOPPED')

    def test_partial_unit_switch_recovers_all_original_files(self):
        path, pin = self.approval()
        original = self.m.os.replace
        def interrupted(source, target):
            if Path(target) == self.units / 'ky-ems.service':
                raise RuntimeError('unit switch interruption')
            return original(source, target)
        with mock.patch.object(self.m.os, 'replace', side_effect=interrupted):
            with self.assertRaisesRegex(RuntimeError, 'unit switch interruption'):
                self.runtime.apply(path, pin)
        self.runtime.recover('TX_ONE', pin)
        self.assertEqual(self.m.tree(self.home), self.old_tree)
        self.assert_stopped_only()

    def killed_switch(self, operation, destination):
        path, pin = self.approval()
        child = os.fork()
        if child == 0:
            original = getattr(self.m.os, operation)
            def kill_after_rename(source, target):
                result = original(source, target)
                if destination(Path(target)):
                    os.kill(os.getpid(), signal.SIGKILL)
                return result
            try:
                with mock.patch.object(self.m.os, operation, side_effect=kill_after_rename):
                    self.runtime.apply(path, pin)
            except BaseException:
                os._exit(98)
            os._exit(99)
        _, result = os.waitpid(child, 0)
        self.assertTrue(os.WIFSIGNALED(result), result)
        self.assertEqual(os.WTERMSIG(result), signal.SIGKILL)
        self.runtime.recover('TX_ONE', pin)
        self.assertEqual(self.m.tree(self.home), self.old_tree)
        self.assertEqual(self.m.identity(self.home), self.old_id)
        self.assertFalse((self.journal / 'pending.json').exists())
        self.assert_stopped_only()

    def test_sigkill_after_old_rename_before_journal_update(self):
        self.killed_switch('rename', lambda path: path == self.journal / 'transactions/TX_ONE/previous-home')

    def test_sigkill_after_new_rename_before_journal_update(self):
        self.killed_switch('rename', lambda path: path == self.home)

    def test_sigkill_during_unit_file_switch(self):
        self.killed_switch('replace', lambda path: path == self.units / 'gateway-services.service')

    def test_sigkill_after_pointer_publish_before_pending_unlink(self):
        self.killed_switch('replace', lambda path: path == self.journal / 'current.json')

    def killed_recovery(self, operation, destination):
        transaction, pin = self.install()
        child = os.fork()
        if child == 0:
            owner = self.m.Path if operation == 'unlink' else self.m.os
            original = getattr(owner, operation)
            def kill_after_change(*args, **kwargs):
                result = original(*args, **kwargs)
                if destination(Path(args[-1])):
                    os.kill(os.getpid(), signal.SIGKILL)
                return result
            try:
                with mock.patch.object(owner, operation, kill_after_change):
                    self.runtime.recover('TX_ONE', pin)
            except BaseException:
                os._exit(98)
            os._exit(99)
        _, result = os.waitpid(child, 0)
        self.assertTrue(os.WIFSIGNALED(result), result)
        self.assertEqual(os.WTERMSIG(result), signal.SIGKILL)
        self.runtime.recover('TX_ONE', pin)
        self.runtime.recover('TX_ONE', pin)
        self.assertEqual(self.m.tree(self.home), self.old_tree)
        self.assertEqual(self.m.identity(self.home), self.old_id)
        self.assertTrue((transaction / 'retired-candidate').is_dir())
        self.assertFalse((self.journal / 'pending.json').exists())
        self.assertFalse((self.journal / 'current.json').exists())
        self.assert_stopped_only()

    def test_sigkill_recovery_after_candidate_retirement(self):
        self.killed_recovery('rename', lambda path: path.name == 'retired-candidate')

    def test_sigkill_recovery_after_original_restoration(self):
        self.killed_recovery('rename', lambda path: path == self.home)

    def test_sigkill_recovery_during_unit_restoration(self):
        self.killed_recovery('replace', lambda path: path == self.units / 'gateway-services.service')

    def test_sigkill_recovery_after_pointer_removal(self):
        self.killed_recovery('unlink', lambda path: path == self.journal / 'current.json')

    def test_sigkill_recovery_after_pending_removal(self):
        self.killed_recovery('unlink', lambda path: path == self.journal / 'pending.json')

    def test_old_image_tamper_refuses_recovery_before_retiring_new_image(self):
        transaction, pin = self.install()
        new_id = self.m.identity(self.home)
        self.write(transaction / 'previous-home/bin/gateway-services.sh', b'foreign edit', 0o755)
        with self.assertRaisesRegex(ValueError, 'original image changed'):
            self.runtime.recover('TX_ONE', pin)
        self.assertEqual(self.m.identity(self.home), new_id)
        self.assertFalse((transaction / 'retired-candidate').exists())

    def test_foreign_unit_edit_is_not_overwritten_by_recovery(self):
        _, pin = self.install()
        new_id = self.m.identity(self.home)
        self.write(self.units / 'ky-ems.service', b'[Service]\nExecStart=/foreign\n')
        with self.assertRaisesRegex(ValueError, 'foreign or missing unit'):
            self.runtime.recover('TX_ONE', pin)
        self.assertEqual(self.m.identity(self.home), new_id)
        self.assertIn(b'/foreign', (self.units / 'ky-ems.service').read_bytes())

    def test_replayed_old_recovery_cannot_roll_back_newer_transaction(self):
        _, first_pin = self.install()
        self.write(self.image / 'bin/gateway-services.sh', b'third launcher\n', 0o755)
        _, second_pin = self.install('TX_TWO')
        new_id = self.m.identity(self.home)
        calls = len(self.calls)
        with self.assertRaisesRegex(ValueError, 'not the current transaction'):
            self.runtime.recover('TX_ONE', first_pin)
        self.assertEqual(len(self.calls), calls)
        self.assertEqual(self.m.identity(self.home), new_id)
        self.runtime.recover('TX_TWO', second_pin)
        self.assertEqual(self.m.read_record(self.journal / 'current.json')['transaction'], 'TX_ONE')

    def test_new_image_must_not_include_queued_state(self):
        self.write(self.image / 'data/pending-controls.json', b'{}\n')
        with self.assertRaisesRegex(ValueError, 'must not carry runtime state'):
            self.approval()

    def test_identity_change_is_rejected(self):
        self.write(self.image / 'config/runtime/device_identity.json', b'{"machineCode":"FOREIGN"}\n')
        with self.assertRaisesRegex(ValueError, 'preserve device identity'):
            self.approval()

    def test_identity_permissions_cannot_be_widened(self):
        (self.home / 'config/runtime/device_identity.json').chmod(0o600)
        with self.assertRaisesRegex(ValueError, 'bytes/mode changed'):
            self.approval()

    def test_setuid_candidate_rejected(self):
        (self.image / 'bin/gateway-services.sh').chmod(0o4755)
        with self.assertRaisesRegex(ValueError, 'ownership/mode unsafe'):
            self.approval()

    def test_service_stop_condition_cannot_be_reset(self):
        self.write(self.new_units / 'ky-ems.service.d/zzzz-reset.conf', b'[Unit]\nConditionPathExists=\\\n  \n')
        with self.assertRaisesRegex(ValueError, 'resets'):
            self.approval()

    def test_other_empty_condition_cannot_reset_stop_fence(self):
        for name in ('ConditionArchitecture', 'ConditionHost', 'ConditionEnvironment'):
            with self.subTest(condition=name):
                self.write(self.new_units / 'ky-ems.service.d/zzzz-reset.conf',
                           ('[Unit]\n' + name + '=\n').encode())
                with self.assertRaisesRegex(ValueError, 'resets'):
                    self.approval()

    def test_unknown_service_payload_is_rejected(self):
        self.write(self.new_units / 'ssh.service', b'[Service]\nExecStart=/foreign\n')
        with self.assertRaisesRegex(ValueError, 'ownership'):
            self.approval()

    def test_internal_scada_symlink_survives_image_switch(self):
        self.write(self.image / 'scada/versions/one/manifest.json', b'{}\n')
        (self.image / 'scada/current').symlink_to('versions/one')
        transaction, pin = self.install()
        self.assertEqual(os.readlink(str(self.home / 'scada/current')), 'versions/one')
        self.runtime.recover('TX_ONE', pin)
        self.assertTrue((transaction / 'retired-candidate/scada/current').is_symlink())

    def test_external_symlink_and_hardlink_are_rejected(self):
        link = self.image / 'bin/external'
        link.symlink_to('/etc/passwd')
        with self.assertRaisesRegex(ValueError, 'symlink escapes'):
            self.approval()
        link.unlink()
        os.link(str(self.image / 'bin/gateway-services.sh'), str(link))
        with self.assertRaisesRegex(ValueError, 'bound/type'):
            self.approval()


if __name__ == '__main__':
    unittest.main()
