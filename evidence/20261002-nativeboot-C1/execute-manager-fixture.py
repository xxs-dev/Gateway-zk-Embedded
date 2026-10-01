"""23-only review runner. Default performs local pin/grammar checks, NO connection."""
import argparse
import ast
import hashlib
import importlib.util
import json
from pathlib import Path, PurePosixPath
import stat
import subprocess

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
COORD = Path('D:/workspace/GatewaySuite-workspaces/realese1.0/coordinator/docs/audits/evidence/20261001-device-acceptance')
PAYLOAD_COMMIT = '9e1fd55298ec503db46e9faffec55cb808798e3b'
MANIFEST_SHA = '95a272548b82f639fd7aba7ea15129dd957acc8cf25a3beb41264fed7c6f3c04'
BASE_COMMIT = 'aa35299d3f418b9dd6c12061a0075181dfa24985'
BASE_PATH = 'evidence/20261002-nativeboot-B2/execute-native-creator.py'
BASE_SHA = 'fee48c39ca46a3fc40cefed0782232183f582c397061ebac33979a117e52770a'
ROOT = '/tmp/gw-nativeboot-082071e-23-C1'
HOST = '192.168.22.23'

UNIT_BOUNDARY = r'''
original_baseline=baseline
def timed_out(signum,frame):raise TimeoutError('C bounded stage deadline')
signal.signal(signal.SIGALRM,timed_out)
def baseline():
    signal.setitimer(signal.ITIMER_REAL,30)
    try:return original_baseline()
    finally:signal.setitimer(signal.ITIMER_REAL,0)

def manager_call(command,seconds):
    events=result.setdefault('unitCleanupCommands',[])
    number=len(events);event=dict(argv=command,timeoutSeconds=seconds,outcome='UNKNOWN')
    events.append(event);save()  # Reserve before manager action; keep uncertain outcome.
    try:
        response=subprocess.run(command,stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=seconds,
                                env={'PATH':'/usr/bin:/bin','LC_ALL':'C'})
        out,err=response.stdout,response.stderr
        assert len(out)+len(err)<=16384,'unit cleanup output limit'
        event.update(outcome='RETURNED',exitCode=response.returncode,stdout=out.decode(),stderr=err.decode())
        return response
    except BaseException as error:
        out=getattr(error,'stdout',None) or b'';err=getattr(error,'stderr',None) or b''
        event.update(errorType=type(error).__name__,errorMessage=str(error),
                     partialStdout=out[:8192].decode('utf-8','replace'),partialStderr=err[:8192].decode('utf-8','replace'))
        raise
    finally:save()

def fixed_unit_code():
    import importlib.util
    spec=importlib.util.spec_from_file_location('c1_unit_boundary',source/'manager-child.py')
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    assert str(module.ROOT)==ROOT
    return module

def manager_properties(unit):
    command=['/usr/bin/systemctl','show',unit,'--no-pager',
             '--property=Id,LoadState,ActiveState,MainPID,ControlGroup,Job,Transient,ExecStart,FragmentPath']
    raw=manager_call(command,2)
    assert raw.returncode==0 and not raw.stderr and len(raw.stdout)<=16384,'manager properties unknown'
    value=dict(line.split('=',1) for line in raw.stdout.decode().splitlines())
    assert value.get('Id')==unit,'manager unit identity drift'
    return {'argv':command,'exitCode':raw.returncode,'stdout':raw.stdout.decode(),'properties':value}

def initial_unit_absence():
    module=fixed_unit_code();expected=module.planned_commands()
    rows=[]
    for unit in expected:
        row=manager_properties(unit);p=row['properties']
        assert p['LoadState']=='not-found' and p['ActiveState']=='inactive' and p['MainPID']=='0' and not p['Job'] and not p['ControlGroup'],'C unit namespace occupied; never touch'
        rows.append(row)
    result['initialUnitAbsence']=rows

def stop_owned_manager_units():
    # Never touch an occupied pre-existing unit, or any unproved argv/fragment.
    if 'initialUnitAbsence' not in result:return ['unit absence admission incomplete']
    import re,importlib.util
    spec=importlib.util.spec_from_file_location('c1_cleanup_health',source/'ota-health-result.py')
    health=importlib.util.module_from_spec(spec);spec.loader.exec_module(health)
    signal.setitimer(signal.ITIMER_REAL,5)
    try:mounted=health.RuntimeHandoff.cgroup_mount()
    finally:signal.setitimer(signal.ITIMER_REAL,0)
    expected=fixed_unit_code().planned_commands();rows=[];unproved=[]
    for unit,argv in expected.items():
        signal.setitimer(signal.ITIMER_REAL,12)
        try:
            before=manager_properties(unit);p=before['properties']
            if p['LoadState']!='not-found':
                assert p['Transient']=='yes' and p['FragmentPath']=='/run/systemd/transient/'+unit,'foreign unit fragment'
                command=p['ExecStart']
                assert command.count('{')==1 and command.count('}')==1,'unexpected command inventory'
                match=re.search(r'\bargv\[\]=(.*?) ;',command)
                assert match and match.group(1)==' '.join(argv),'unknown unit argv; refuse stop'
                stopped=manager_call(['/usr/bin/systemctl','stop',unit],3)
                assert stopped.returncode==0 and len(stopped.stdout)+len(stopped.stderr)<=16384,'private unit stop unknown'
                cleared=manager_call(['/usr/bin/systemctl','reset-failed',unit],2)
                # GC after stop may unload a successful transient unit.
                assert cleared.returncode in (0,1,5) and len(cleared.stdout)+len(cleared.stderr)<=16384
            after=manager_properties(unit);p=after['properties']
            assert p['MainPID']=='0' and not p['Job'] and p['ActiveState'] in ('inactive','failed'),'manager owner remains'
            proof=health.RuntimeHandoff.__new__(health.RuntimeHandoff);proof.unit=unit
            proof.no_unit_tasks(dict(bootId=proof.boot(),cgroupMount=mounted,unitControlGroup='/system.slice/'+unit))
            rows.append(dict(unit=unit,before=before,after=after,noCgroupTasks=True))
        except BaseException as error:
            unproved.append(dict(unit=unit,errorType=type(error).__name__,errorMessage=str(error)))
        finally:
            signal.setitimer(signal.ITIMER_REAL,0)
            result['unitCleanupProof']=rows;result['remainingManagerUnits']=unproved;save()
    result['unitCleanupProof']=rows
    return unproved
'''


def git_blob(commit, path):
    return subprocess.check_output(['git', 'show', commit + ':' + path], cwd=str(REPO))


def base_executor():
    raw = git_blob(BASE_COMMIT, BASE_PATH)
    assert hashlib.sha256(raw).hexdigest() == BASE_SHA
    module = {'__name__': 'c1_pinned_b2_primitives', '__file__': str(REPO / BASE_PATH)}
    exec(compile(raw, BASE_PATH, 'exec'), module)
    return module


def local_unit_plan(data):
    tree = ast.parse(data)
    names = {'ROOT', 'PREFIX', 'TOKEN', 'JOBS', 'CHECKS'}
    functions = {'worker_identifier', 'unit_names', 'planned_commands'}
    nodes = [node for node in tree.body if
             isinstance(node, ast.Assign) and any(isinstance(t, ast.Name) and t.id in names for t in node.targets) or
             isinstance(node, ast.FunctionDef) and node.name in functions]
    context = dict(Path=PurePosixPath, hashlib=hashlib, json=json)
    exec(compile(ast.Module(body=nodes, type_ignores=[]), 'C1-pure-fixed-unit-plan', 'exec'), context)
    assert str(context['ROOT']) == ROOT
    units = context['unit_names'](); commands = context['planned_commands']()
    assert len(units) == 9 and len(set(units)) == 9 and set(commands) == set(units)
    return units, commands


def remote_code(base):
    code = base['RUN_CODE']
    # This is a version-bound adaptation of the already reviewed group/baseline
    # runner, not a new product state machine. Each anchor must exist exactly once.
    changes = [
        ("str(source/'native-child.py')", "str(source/'manager-child.py')"),
        ("summary=root/'native-result.json'", "summary=root/'manager-result.json'"),
        ("assert os.geteuid()==0,'private native validation requires root'", "assert os.geteuid()==0,'private manager validation requires root'"),
        ("ws=work.stat();work_identity=(ws.st_dev,ws.st_ino)", "ws=work.stat();work_identity=(ws.st_dev,ws.st_ino)\n    initial_unit_absence()"),
        ("try:remaining=terminate_group();result['remainingFixtureProcesses']=remaining", "try:remaining=terminate_group();result['remainingFixtureProcesses']=remaining\n    except BaseException as error:result['groupCleanupError']=str(error);remaining=['unverified']\n    try:result['remainingManagerUnits']=stop_owned_manager_units()\n    except BaseException as error:result['unitCleanupError']=str(error);result['remainingManagerUnits']=['unverified']\n    try:pass")]
    for old, new in changes:
        assert code.count(old) == 1, 'B2 reuse anchor changed: ' + old
        code = code.replace(old, new)
    code = code.replace('native', 'manager').replace('privateNativeOnly', 'privateManagerOnly')
    assert code.count("{'checks':6,'passed':6}") == 2
    code = code.replace("{'checks':6,'passed':6}", "{'checks':10,'passed':10}")
    # Detached systemd workers require a separate positive absence proof.
    code = code.replace("if work_identity and not remaining and", "if work_identity and not result.get('remainingManagerUnits') and not remaining and")
    code = code.replace("result.get('remainingFixtureProcesses')==[] and result.get('workRemoved')", "result.get('remainingFixtureProcesses')==[] and result.get('remainingManagerUnits')==[] and result.get('workRemoved')")
    code = code.replace("('errorType','postcheckError','groupCleanupError','timedOut')", "('errorType','postcheckError','groupCleanupError','unitCleanupError','timedOut')")
    code = code.replace('resource.RLIMIT_CPU,180', 'resource.RLIMIT_CPU,90')
    code = code.replace('timeoutSeconds=180', 'timeoutSeconds=90').replace('child.wait(timeout=180)', 'child.wait(timeout=90)')
    assert code.count('verified=[]') == 1 and code.count("result['verifiedInputs']=verified") == 1
    code = code.replace('verified=[]', 'signal.setitimer(signal.ITIMER_REAL,15)\n    verified=[]')
    code = code.replace("result['verifiedInputs']=verified", "result['verifiedInputs']=verified\n    signal.setitimer(signal.ITIMER_REAL,0)")
    # The reused body imports signal before this prefix is evaluated.
    prefix = 'import signal\n' + UNIT_BOUNDARY
    ast.parse(prefix + code, feature_version=(3, 8))
    return prefix + code


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--execute-approved', action='store_true')
    args = parser.parse_args()
    raw = git_blob(PAYLOAD_COMMIT, 'evidence/20261002-nativeboot-C1/input-manifest.json')
    assert hashlib.sha256(raw).hexdigest() == MANIFEST_SHA
    manifest = json.loads(raw)
    assert manifest['root'] == ROOT and manifest['host'] == HOST and manifest['executionAuthorized'] is False
    pins, blobs = {}, {}
    for row in manifest['files']:
        data = git_blob(PAYLOAD_COMMIT, row['path'])
        assert len(data) == row['bytes'] and hashlib.sha256(data).hexdigest() == row['sha256']
        ast.parse(data.decode(), feature_version=(3, 8))
        name = row['sourceName']; blobs[name] = data
        pins[name] = {k: row[k] for k in ('bytes', 'sha256', 'mode')}
    assert len(pins) == manifest['fileCount'] == 5 and sum(map(len, blobs.values())) == manifest['inputBytes']
    units, commands = local_unit_plan(blobs['manager-child.py'])
    base = base_executor(); code = remote_code(base)
    ast.parse(base['BASELINE_CODE'], feature_version=(3, 8))
    probe = (COORD / 'd2_device_readonly_probe.py').read_bytes()
    helper = (COORD / 'run_bounded_probe.py').read_bytes()
    assert hashlib.sha256(probe).hexdigest() == manifest['connectionProbeSha256']
    assert hashlib.sha256(helper).hexdigest() == manifest['connectionHelperSha256']
    plan = dict(host=HOST, root=ROOT, payloadCommit=PAYLOAD_COMMIT, manifestSha256=MANIFEST_SHA,
                inputs=pins, checks=manifest['checks'], remoteCodeSha256=hashlib.sha256(code.encode()).hexdigest(),
                units=units, plannedUnitCommands=commands,
                executeApprovedFlag=args.execute_approved, remoteAuthorized=False, privateManagerOnly=True,
                nativeOrBusinessLifecycle=False)
    if not args.execute_approved:
        print(json.dumps(plan, indent=2)); return 0  # No connection/helper import/manager or fixture execution.
    receipt = HERE / 'board-23-C1-result.json'
    with receipt.open('x') as output:
        output.write('{"phase":"LOCAL_RESERVED","sshStarted":false}\n')
    result = dict(plan, sshStarted=False); client = None
    try:
        spec = importlib.util.spec_from_file_location('c1_existing_connect', COORD / 'run_bounded_probe.py')
        connection = importlib.util.module_from_spec(spec); spec.loader.exec_module(connection)
        client = connection.connect(HOST); result['sshStarted'] = True
        prefix = 'PROBE=' + repr(probe.decode()) + '\nNAMES=()\n' + base['BASELINE_CODE']
        status, out, err = base['invoke'](client, prefix + '\nprint(json.dumps(baseline()))\n', 60)
        result.update(preflightExitCode=status, preflightStdout=out.decode(), preflightStderr=err.decode())
        assert status == 0 and not err; expected = json.loads(out)
        with client.open_sftp() as sftp:
            assert stat.S_ISDIR(sftp.lstat('/tmp').st_mode)
            try: sftp.lstat(ROOT)
            except FileNotFoundError: pass
            else: raise ValueError('unique C root occupied; no retry')
            sftp.mkdir(ROOT, mode=0o700); sftp.mkdir(ROOT + '/source', mode=0o700)
            result['uploadStarted'] = True
            for name, data in sorted(blobs.items()):
                with sftp.open(ROOT + '/source/' + name, 'wx') as output: output.write(data)
                sftp.chmod(ROOT + '/source/' + name, pins[name]['mode'])
            sftp.chmod(ROOT + '/source', 0o500)
        params = dict(HOST=HOST, ROOT=ROOT, COMMIT=PAYLOAD_COMMIT, PINS=pins,
                      CHECKS=manifest['checks'], NAMES=(), EXPECTED=expected)
        status, out, err = base['invoke'](client, 'globals().update(' + repr(params) + ')\n' + prefix + code, 420)
        result.update(remoteExitCode=status, remoteStdout=out.decode(), remoteStderr=err.decode())
        if out: result['execution'] = json.loads(out)
    except BaseException as error:
        result.update(errorType=type(error).__name__, errorMessage=str(error))
    finally:
        if client is not None: client.close()
        result['sshClosed'] = True
        receipt.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({k: v for k, v in result.items() if k not in ('execution', 'preflightStdout', 'remoteStdout')}))
    return 0 if result.get('remoteExitCode') == 0 and not result.get('remoteStderr') and 'errorType' not in result else 1


if __name__ == '__main__':
    raise SystemExit(main())
