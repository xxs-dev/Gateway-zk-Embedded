import contextlib
import copy
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import event_store_104_tune as t


class TuneTests(unittest.TestCase):
    def run_fixture(self, fail=False):
        with tempfile.TemporaryDirectory() as tmp,contextlib.ExitStack() as stack:
            work=Path(tmp);app=work/'app.json';identity=work/'identity.json'
            original={'mqttDriver':{'scanIntervalMs':1000,'fullUploadIntervalMs':30000},'compute':{'unchanged':True}}
            t.d.save(app,original);t.d.save(identity,{'machineCode':t.d.IDENTITY})
            plan={'spec':{'identity':str(identity)},'configs':{str(app):{'activated':str(app)}},'hashes':{}}
            state={'phase':'active'}
            t.d.save(work/'plan.json',plan);t.d.save(work/'state.json',state)
            calls=[];count=[0]
            def ctl(*args):
                calls.append(args)
                if 'start' in args:
                    count[0]+=1
                    if fail and count[0]==1:raise RuntimeError('start failed')
                return ''
            stack.enter_context(patch.object(t,'WORK',work))
            stack.enter_context(patch.object(t.d,'load',return_value=(copy.deepcopy(plan),state)))
            stack.enter_context(patch.object(t.d,'verify',return_value={'phase':'active'}))
            stack.enter_context(patch.object(t.d,'stopped'))
            stack.enter_context(patch.object(t.d,'systemctl',side_effect=ctl))
            if fail:
                with self.assertRaisesRegex(RuntimeError,'start failed'):t.main()
                self.assertEqual(t.d.read_json(app),original)
                self.assertEqual(t.d.read_json(work/'plan.json'),plan)
            else:
                t.main()
                modified=t.d.read_json(app)
                self.assertEqual(t.d.diff_paths(original,modified),[('mqttDriver','deliveryMaxLatencyMs')])
                self.assertEqual(modified['mqttDriver']['deliveryMaxLatencyMs'],200)
                self.assertEqual(t.d.digest(work/'plan.json'),t.d.read_json(work/'state.json')['plan_sha256'])
            self.assertTrue(all(args[-1] in t.UNITS for args in calls))

    def test_only_delivery_budget_changes_with_valid_new_seal(self):self.run_fixture()
    def test_start_failure_restores_config_without_restoring_db(self):self.run_fixture(True)


if __name__=='__main__':unittest.main()
