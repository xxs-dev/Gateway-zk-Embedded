"""Private filesystem/fault-injection coverage; no real service or SHM changes."""
import importlib.util
import json
import os
from pathlib import Path
import time
import unittest
from unittest import mock


@unittest.skipUnless(getattr(os, 'geteuid', lambda: -1)() == 0, 'root-owned private fixture')
class BootstrapJournalTest(unittest.TestCase):
    def setUp(self):
        spec = importlib.util.spec_from_file_location('journal_fixture', Path(__file__).with_name('gateway_reprovision_test.py'))
        m = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(m)
        self.f = m.ReprovisionTest('test_install_expands_units_and_keeps_original_image')
        self.f.setUp()
        self.addCleanup(self.f.doCleanups)
        f = self.f
        self.journal = f.root/'bootstrap'
        f.patch(f.m, 'BOOTSTRAP_ROOT', self.journal, create=True)
        f.install('ORIGIN')
        self.write(self.journal/'transactions/PREP/approval.json', {'format':'runtime-bootstrap-prepare-v1'})
        prep_pin = f.m.digest(self.journal/'transactions/PREP/approval.json')
        self.write(self.journal/'transactions/PREP/ready.json', {'format':'runtime-bootstrap-prepared-v1'})
        ready_pin = f.m.digest(self.journal/'transactions/PREP/ready.json')
        self.write(self.journal/'current.json', dict(transaction='PREP',approvalSha256=prep_pin,readySha256=ready_pin))
        baseline = dict(baselineId='BASE')
        self.write(f.home/'data/runtime-production/baselines/BASE.json', baseline)
        self.write(f.home/'data/runtime-production/current.json',dict(transaction='ACT',baselineId='BASE',
            sha256=f.m.digest(f.home/'data/runtime-production/baselines/BASE.json')))
        pointer = f.m.current_pointer()
        self.write(self.journal/'activations/ACT/approval.json',dict(format='runtime-bootstrap-activation-v1',
            operation='bootstrap',transaction='ACT',gatewayHome=str(f.home),
            origin=dict(pointer,imageIdentity=f.m.identity(f.home)),candidate=baseline,
            preparedTransaction='PREP',prepareApprovalSha256=prep_pin,preparedSha256=ready_pin))
        pin = f.m.digest(self.journal/'activations/ACT/approval.json')
        self.write(self.journal/'activations/current.json',dict(transaction='ACT',approvalSha256=pin))
        self.write(self.journal/'activations/ACT/state.json',dict(phase='COMPLETED',transaction='ACT',approvalSha256=pin))
        self.old_tree = f.m.tree(self.journal)
        self.old_id = f.m.identity(self.journal)
        self.home_tree = f.m.tree(f.home)

    def write(self,path,value):
        self.f.write(path,(json.dumps(value,sort_keys=True)+'\n').encode(),0o600)

    def approval(self):
        f = self.f
        value = f.m.plan(f.image,f.new_units,'REPLACE',int(time.time())+600,retire_bootstrap=True)
        path = f.root/'replace.json'
        f.m.record(path,value,new=True)
        return path,f.m.digest(path)

    def test_archive_and_recovery_preserve_entire_journal_and_inode(self):
        f = self.f
        path,pin = self.approval()
        tx = f.runtime.apply(path,pin)
        self.assertFalse(self.journal.exists())
        self.assertEqual(f.m.tree(tx/'previous-bootstrap'),self.old_tree)
        self.assertEqual(f.m.identity(tx/'previous-bootstrap'),self.old_id)
        f.runtime.recover('REPLACE',pin)
        f.runtime.recover('REPLACE',pin)
        self.assertEqual(f.m.tree(self.journal),self.old_tree)
        self.assertEqual(f.m.identity(self.journal),self.old_id)
        self.assertEqual(f.m.tree(f.home),self.home_tree)
        f.assert_stopped_only()

    def test_default_plan_refuses_to_orphan_existing_bootstrap(self):
        f = self.f
        with self.assertRaisesRegex(ValueError,'bootstrap journal'):
            f.m.plan(f.image,f.new_units,'REPLACE',int(time.time())+600)

    def test_pending_and_uncompleted_activation_refused(self):
        f = self.f
        for path in (self.journal/'pending.json',self.journal/'activations/pending.json',f.home/'data/runtime-production/pending.json'):
            self.write(path,{'pending':True})
            with self.assertRaisesRegex(ValueError,'pending'):
                self.approval()
            path.unlink()
        state = self.journal/'activations/ACT/state.json'
        value = f.m.read_record(state)
        value['phase'] = 'STARTING'
        self.write(state,value)
        with self.assertRaisesRegex(ValueError,'completed'):
            self.approval()

    def test_modified_journal_refused_before_new_stop_or_switch(self):
        f = self.f
        path,pin = self.approval()
        self.write(self.journal/'unexpected.json',{'foreign':True})
        calls = list(f.calls)
        with self.assertRaisesRegex(ValueError,'approval drift'):
            f.runtime.apply(path,pin)
        self.assertEqual(f.calls,calls)
        self.assertEqual(f.m.tree(f.home),self.home_tree)

    def test_crash_after_journal_rename_recovers_original_pair(self):
        f = self.f
        path,pin = self.approval()
        rename = f.m.os.rename
        def interrupted(source,destination):
            rename(source,destination)
            if Path(source) == self.journal:
                raise OSError('injected after bootstrap rename')
        with mock.patch.object(f.m.os,'rename',interrupted):
            with self.assertRaisesRegex(OSError,'bootstrap rename'):
                f.runtime.apply(path,pin)
        f.runtime.recover('REPLACE',pin)
        self.assertEqual(f.m.tree(self.journal),self.old_tree)
        self.assertEqual(f.m.tree(f.home),self.home_tree)

    def test_foreign_replacement_journal_blocks_recovery_without_image_switch(self):
        f = self.f
        path,pin = self.approval()
        f.runtime.apply(path,pin)
        candidate = f.m.identity(f.home)
        self.write(self.journal/'foreign.json',{'foreign':True})
        with self.assertRaisesRegex(ValueError,'bootstrap journal'):
            f.runtime.recover('REPLACE',pin)
        self.assertEqual(f.m.identity(f.home),candidate)
        self.assertTrue((self.journal/'foreign.json').exists())

    def test_stop_failure_never_retires_journal(self):
        f = self.f
        path,pin = self.approval()
        f.fail_stop = True
        with self.assertRaisesRegex(ValueError,'stop refusal'):
            f.runtime.apply(path,pin)
        self.assertEqual(f.m.identity(self.journal),self.old_id)
        f.fail_stop = False
        f.runtime.recover('REPLACE',pin)
        self.assertEqual(f.m.tree(self.journal),self.old_tree)

    def test_archived_journal_drift_blocks_recovery_before_image_switch(self):
        f = self.f
        path,pin = self.approval()
        tx = f.runtime.apply(path,pin)
        candidate = f.m.identity(f.home)
        self.write(tx/'previous-bootstrap/foreign.json',{'changed':True})
        with self.assertRaisesRegex(ValueError,'bootstrap journal'):
            f.runtime.recover('REPLACE',pin)
        self.assertEqual(f.m.identity(f.home),candidate)

    def test_restore_rename_interruption_is_idempotently_recoverable(self):
        f = self.f
        path,pin = self.approval()
        tx = f.runtime.apply(path,pin)
        rename = f.m.os.rename
        def interrupted(source,destination):
            rename(source,destination)
            if Path(source) == tx/'previous-bootstrap':
                raise OSError('injected after restore rename')
        with mock.patch.object(f.m.os,'rename',interrupted):
            with self.assertRaisesRegex(OSError,'restore rename'):
                f.runtime.recover('REPLACE',pin)
        f.runtime.recover('REPLACE',pin)
        self.assertEqual(f.m.tree(self.journal),self.old_tree)
        self.assertEqual(f.m.identity(self.journal),self.old_id)
        self.assertEqual(f.m.tree(f.home),self.home_tree)

    def test_bootstrap_origin_refuses_modified_retained_journal(self):
        f = self.f
        path,pin = self.approval()
        tx = f.runtime.apply(path,pin)
        spec = importlib.util.spec_from_file_location('journal_origin',
            Path(__file__).resolve().parents[1]/'deploy/runtime-bootstrap.py')
        m = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(m)
        f.patch(m,'reprovision',f.m)
        f.patch(m,'HOME',f.home)
        f.patch(m,'guard',f.m.guard)
        f.patch(m.guard,'systemctl_value',lambda u,p: f.systemctl('show','--property='+p,'--value',u))
        self.assertEqual(m.origin()['transaction'],'REPLACE')
        self.write(tx/'previous-bootstrap/foreign.json',{'changed':True})
        with self.assertRaisesRegex(ValueError,'bootstrap journal'):
            m.origin()


@unittest.skipUnless(getattr(os, 'geteuid', lambda: -1)() == 0, 'root-owned private fixture')
class RebootstrapIntegrationTest(unittest.TestCase):
    def test_completed_activation_replace_and_prepare_again(self):
        spec = importlib.util.spec_from_file_location('rebootstrap_activation',
            Path(__file__).with_name('runtime_bootstrap_activation_test.py'))
        m = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(m)
        a = m.ActivationTest('test_first_baseline_is_real_generation_one_without_legacy_history')
        a.setUp()
        self.addCleanup(a.doCleanups)
        a.apply()
        f,p = a.f,a.p
        before = f.m.bootstrap_journal_snapshot()
        value = f.m.plan(f.image,f.new_units,'REPLACE',int(time.time())+600,retire_bootstrap=True)
        path = f.root/'replace.json'
        f.m.record(path,value,new=True)
        tx = f.runtime.apply(path,f.m.digest(path))
        self.assertFalse(a.live)
        self.assertEqual(f.m.tree(tx/'previous-bootstrap'),before['tree'])
        self.assertEqual(p.m.origin()['transaction'],'REPLACE')
        for config in (p.source/'apps').glob('*.json'):
            data = json.loads(config.read_text())
            data['mqttDriver']['sharedMemoryName'] = 'replacement_data'
            f.write(config,json.dumps(data).encode())
        p.authority['monitorShmName'] = 'replacement_monitor'
        resolved = p.m.policy.resolve(p.source,f.units,p.authority,p.m.TRUST)
        p.profile.update(allowedUnits=[r['unit'] for r in resolved['roles']],
            segmentNames=sorted({n for r in resolved['roles'] for n in r['shm']['names']}))
        path,pin = p.approval('PREPARE_REPLACEMENT')
        prepared = p.runtime.prepare(path,pin)
        self.assertEqual(f.m.read_record(prepared/'state.json')['phase'],'PREPARED_STOPPED')
        self.assertEqual(f.m.tree(tx/'previous-bootstrap'),before['tree'])
        self.assertFalse(a.live)
        self.assertTrue((p.m.SHM/'fresh_data').exists())
        self.assertTrue((p.m.SHM/'replacement_data').exists())


if __name__ == '__main__':
    unittest.main()
