import copy
from contextlib import ExitStack
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('handover', Path(__file__).with_name('easytier_legacy_systemd.py'))
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)


class HandoverTests(unittest.TestCase):
    def exercise(self, scenario):
        with tempfile.TemporaryDirectory() as tmp, ExitStack() as stack:
            root = Path(tmp); rc = root/'rc.local'; unit = root/'easytier.service'; stage = root/'stage'; stage.mkdir()
            original = b'#!/bin/bash\nif [ -f /usr/local/bin/easytier-core ]; then\n echo start\n'+m.LINE+b'\nfi\n'
            rc.write_bytes(original)
            p = dict(pid=123, start='100', exe=m.EXE, exactArgv=True, cwd='/', cgroup='session.scope',
                     standardEnv={'LANG':'zh_CN.UTF-8'},unknownEnvKeys=[],uid=['0']*4,gid=['0']*4,umask='0022',namespaces={'net':True})
            b = dict(code='COMM202600096', boot='boot', configSha='cfg', exeSha='exe', rcSha=m.sha(rc), rcMode=0o755,
                     rcSymlink=False, rcExecutable=True, rcActive='active', rcLines=1, unitExists=False,
                     dropins=[], linkExists=False, unit={'LoadState':'not-found'}, processes=[p], ports=[123], business={})
            b['patchedSha'] = m.hashlib.sha256(original.replace(m.LINE, m.COMMENT)).hexdigest()
            if scenario=='unknown_env': p['unknownEnvKeys']=['LD_PRELOAD']
            if scenario=='wrong_namespace': p['namespaces']['net']=False
            if scenario=='duplicate': b['processes'].append(dict(p,pid=789))
            (stage/'metadata.json').write_text(json.dumps({'before':b, 'nonce':'nonce'}))
            state = dict(old=True, service=False, clock=0, killed=0, spawn=0, faulted=False)

            def info(name):
                return {'MainPID':'456' if state['service'] else '0', 'ActiveState':'active' if state['service'] else 'inactive'}

            def probe(code):
                s = copy.deepcopy(b)
                if state['service']:
                    s['rcSha']=m.sha(rc);s['rcLines']=0
                    s['unit']=dict(MainPID='456', ActiveState='active', SubState='running', NRestarts='0', UnitFileState='enabled')
                    s['processes']=[dict(p,pid=456,cgroup='/system.slice/easytier.service')];s['ports']=[456]
                return s

            def command(*args):
                if args[:2]==('systemctl','start'):state['service']=True
                if args[:2]==('systemctl','stop'):state['service']=False
                return ''

            def atomic(path, data, mode=0o600):
                selected = (scenario.startswith('rc_') and path==rc) or (scenario.startswith('unit_') and path==unit)
                fault = selected and not state['faulted']
                if fault and scenario.endswith('before'):
                    state['faulted']=True;raise OSError('injected before rename')
                path.write_bytes(data)
                if fault and scenario.endswith('after'):
                    state['faulted']=True;raise OSError('injected after rename')

            def kill(pid,sig):
                self.assertEqual(pid,123);state['killed']+=1
                if scenario!='refuses_term':state['old']=False

            def sleep(seconds):
                state['clock']+=seconds
                if state['service'] and scenario in ('success','wrong_ack'):
                    (stage/'ack.json').write_text(json.dumps({'nonce':'nonce','pid':999 if scenario=='wrong_ack' else 456}))

            stack.enter_context(patch.object(m,'RC',rc));stack.enter_context(patch.object(m,'UNIT',unit))
            for name, value in [('probe',probe),('atomic',atomic),('cmd',command),('unit_info',info),
                                ('same_process',lambda p:state['old']),('port_owners',lambda:[123] if state['old'] else ([777] if scenario=='unknown_listener' else ([456] if state['service'] else []))),('sync_dir',lambda p:None)]:
                stack.enter_context(patch.object(m,name,value))
            stack.enter_context(patch.object(m.os,'kill',kill))
            stack.enter_context(patch.object(m.time,'monotonic',lambda:state['clock']))
            stack.enter_context(patch.object(m.time,'sleep',sleep))
            stack.enter_context(patch.object(m.subprocess,'run',lambda *a,**kw:None))
            stack.enter_context(patch.object(m.subprocess,'Popen',lambda *a,**kw:state.update(spawn=state['spawn']+1)))
            if scenario in ('unknown_env','wrong_namespace','duplicate'):
                with self.assertRaises(AssertionError): m.runner(stage)
                self.assertEqual(state['killed'],0);self.assertEqual(rc.read_bytes(),original);self.assertFalse(unit.exists())
                return
            m.runner(stage)
            result=json.loads((stage/'result.json').read_text())
            if scenario=='success':
                self.assertEqual(result['status'],'COMMITTED');self.assertTrue(unit.exists());self.assertTrue(state['service'])
            elif scenario=='unknown_listener':
                self.assertEqual(result['status'],'ROLLBACK_FAILED');self.assertEqual(state['spawn'],0)
                self.assertFalse(unit.exists());self.assertEqual(rc.read_bytes(),original)
            else:
                self.assertEqual(result['status'],'ROLLED_BACK');self.assertFalse(unit.exists());self.assertEqual(rc.read_bytes(),original)
                self.assertFalse(state['service']);self.assertEqual(state['spawn'],0 if state['old'] else 1)

    def test_success(self):self.exercise('success')
    def test_no_ack(self):self.exercise('timeout')
    def test_wrong_ack(self):self.exercise('wrong_ack')
    def test_refuses_term(self):self.exercise('refuses_term')
    def test_rc_before(self):self.exercise('rc_before')
    def test_rc_after(self):self.exercise('rc_after')
    def test_unit_before(self):self.exercise('unit_before')
    def test_unit_after(self):self.exercise('unit_after')
    def test_unknown_env(self):self.exercise('unknown_env')
    def test_wrong_namespace(self):self.exercise('wrong_namespace')
    def test_duplicate(self):self.exercise('duplicate')
    def test_unknown_listener(self):self.exercise('unknown_listener')
    def test_unsafe_cwd(self):
        for cwd in ('/tmp/a%h','/tmp/a\nb','relative'):
            with self.assertRaises(AssertionError):m.unit_bytes(cwd)


if __name__=='__main__':unittest.main()
