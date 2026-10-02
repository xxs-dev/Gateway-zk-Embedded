"""Fixed P2 single read-only request; default has no authentication/connection."""
import argparse
import ast
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
SOURCE = '438507cdb191b5b91c77eca2e672b4e74e06bd1f'
BASE = 'evidence/20261002-nativeboot-B2/execute-native-creator.py'
BASE_SHA = 'fee48c39ca46a3fc40cefed0782232183f582c397061ebac33979a117e52770a'
COORD = Path('D:/workspace/GatewaySuite-workspaces/realese1.0/coordinator/docs/audits/evidence/20261001-device-acceptance')
CONNECT_SHA = 'd8dfd569062fee7bbb022c6f4f49b93bdd4a0415a819d1c2e5bfcb16f2dc7ccf'
PROBE_SHA = '509a74fcf5658156773d1273b4e2222d5b8d687da57bc8cdaf0086a88765a221'
HOST = '192.168.22.23'


def sha(data): return hashlib.sha256(data).hexdigest()


def prepare():
    files = {}
    for name in ('execute-proc-readonly.py', 'proc-readonly.py', 'proc_readonly_test.py'):
        data = (HERE/name).read_bytes(); ast.parse(data.decode(), feature_version=(3,8))
        files[name] = dict(bytes=len(data), sha256=sha(data))
    raw = subprocess.check_output(['git','cat-file','blob',SOURCE+':'+BASE], cwd=str(REPO))
    assert sha(raw) == BASE_SHA
    base = {'__name__':'r10_P2_baseline','__file__':str(REPO/BASE)}
    exec(compile(raw, BASE, 'exec'), base)
    probe = (COORD/'d2_device_readonly_probe.py').read_bytes()
    assert sha(probe) == PROBE_SHA and sha((COORD/'run_bounded_probe.py').read_bytes()) == CONNECT_SHA
    manifest = dict(host=HOST, files=files, executionAuthorized=False, remoteWrites=0,
                    namespaceCreated=False, signalsSent=0, baselineGitSha256=BASE_SHA,
                    probeSha256=PROBE_SHA, connectSha256=CONNECT_SHA,
                    b1WrapperSha256='911cb14129824dfb7772e62cc15dbac26bf69d0c26b20ccde5685a3dbb4f61fc',
                    b1ReceiptSha256='acf7d4d7d85a1e61563cb4b094e952ef701a3e0f5760e30044c38d0718d25d2f',
                    namespaces=['pid:[4026532696]','mnt:[4026532695]','net:[4026532698]'])
    prelude = 'PROBE='+repr(probe.decode())+'\nNAMES=()\n'+base['BASELINE_CODE']
    code = prelude+'\n'+(HERE/'proc-readonly.py').read_text()+'''
print(json.dumps(probe(baseline)))
'''
    code = code.replace('print(json.dumps(probe(baseline)))',
                        'result=probe(baseline)\nprint(json.dumps(result))\nraise SystemExit(0 if result["outcome"]=="NO_REFERENCES_OBSERVED" else 1)')
    ast.parse(code, feature_version=(3,8))
    return manifest, code, base


def main():
    parser = argparse.ArgumentParser(); parser.add_argument('--freeze', action='store_true')
    parser.add_argument('--execute-approved', action='store_true'); parser.add_argument('--manifest-sha256')
    args = parser.parse_args(); assert not(args.freeze and args.execute_approved)
    manifest, code, base = prepare(); encoded=(json.dumps(manifest,sort_keys=True,indent=2)+'\n').encode()
    if args.freeze:
        with (HERE/'input-manifest.json').open('xb') as output: output.write(encoded)
    else: assert (HERE/'input-manifest.json').read_bytes()==encoded
    digest=sha(encoded)
    if args.manifest_sha256 is not None: assert digest==args.manifest_sha256
    plan=dict(host=HOST,manifestSha256=digest,connects=args.execute_approved,remoteWrites=0,signalsSent=0,namespaceCreated=False)
    if not args.execute_approved: print(json.dumps(plan,indent=2)); return 0
    assert args.manifest_sha256 is not None
    receipt=HERE/'P2-result.json'
    with receipt.open('x') as output: output.write('{"phase":"LOCAL_RESERVED","sshStarted":false}\n')
    result=dict(plan,sshStarted=False); client=None
    try:
        spec=importlib.util.spec_from_file_location('r10_P2_connect',COORD/'run_bounded_probe.py')
        connection=importlib.util.module_from_spec(spec); spec.loader.exec_module(connection)
        client=connection.connect(HOST);result['sshStarted']=True
        status,raw,error=base['invoke'](client,code,90)
        result.update(remoteExitCode=status,stdout=raw.decode('utf-8','replace'),stderr=error.decode('utf-8','replace'))
        if raw: result['execution']=json.loads(raw)
    except BaseException as error: result['localError']=dict(type=type(error).__name__,message=str(error))
    finally:
        try:
            if client is not None:client.close()
            result['sshClosed']=True
        except BaseException as error:result.update(sshClosed=False,closureError=str(error))
        receipt.write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps({k:v for k,v in result.items() if k not in ('stdout','execution')}))
    return 0 if result.get('remoteExitCode')==0 and result.get('sshClosed') and not result.get('stderr') and not any(k in result for k in ('localError','closureError')) else 1


if __name__=='__main__':raise SystemExit(main())
