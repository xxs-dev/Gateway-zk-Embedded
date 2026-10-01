"""Review-only until --execute-approved; 23-only private Python fixture, no host lifecycle."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import stat
import subprocess

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
COORD = Path('D:/workspace/GatewaySuite-workspaces/realese1.0/coordinator/docs/audits/evidence/20261001-device-acceptance')
COMMIT = '7e93c745a209bb2b9ef64e788d3d287373b20d55'
HOST = '192.168.22.23'
ROOT = '/tmp/gw-production-checkpoint-7e93c74-23-r1'
PROBE_SHA = '509a74fcf5658156773d1273b4e2222d5b8d687da57bc8cdaf0086a88765a221'
CONNECT_SHA = 'd8dfd569062fee7bbb022c6f4f49b93bdd4a0415a819d1c2e5bfcb16f2dc7ccf'
MANIFEST_SHA = '1764d4f86b3405fbd8085a25ed0bf5298c54db735f9ad05ce1f97085840f743c'
FILES = ('deploy/offline-runtime-upgrade.py', 'deploy/runtime-upgrade-guard.py',
         'deploy/ota-health-result.py', 'deploy/gateway-services.sh', 'deploy/ota-rollback.sh',
         'deploy/gateway-ota-recover.service', 'tools/production_runtime_baseline_test.py',
         'tools/fixtures/production-historical-r2-allowlists.json')
CASES = ('test_failed_rollback_completed_then_owner_final_crash_resumes_original_job',
         'test_boot_hook_resolves_durable_owner_tail_even_without_runtime_pending',
         'test_boot_oneshot_during_start_requires_positive_live_offline_owner',
         'test_dangling_production_pointer_cannot_fall_back_to_legacy_authority',
         'test_mismatched_effective_dropin_or_fragment_denies_before_intent',
         'test_payload_wrong_elf_architecture_denies_before_any_intent',
         'test_boot_malformed_pending_ref_denies_before_recovery_or_mutation',
         'test_process_death_mid_copy_is_recovered_without_ignoring_inventory')
BASELINE_CODE = r'''
import contextlib,io,json,subprocess
def baseline():
    capture=io.StringIO();raw_commands=[];original=subprocess.check_output
    def observed(*a,**k):
        value=original(*a,**k)
        assert len(value)<=16384
        raw_commands.append({'argv':a[0],'exitCode':0,'stdout':value})
        return value
    try:
        subprocess.check_output=observed
        with contextlib.redirect_stdout(capture):exec(PROBE,{})
    finally:subprocess.check_output=original
    raw=capture.getvalue();assert len(raw.encode())<=131072
    value=json.loads(raw)
    assert value['machineCode']=='COMM202600999' and value['architecture']=='aarch64'
    return {'snapshot':value,'rawJson':raw,'rawCommands':raw_commands}
'''
RUN_CODE = r'''
import ctypes,hashlib,os,resource,shutil,signal,stat,time
from pathlib import Path
root=Path(ROOT);source=root/'source';work=root/'work'
result={'host':HOST,'commit':COMMIT,'root':ROOT,'fixtureOnly':True,'realLifecycleCalled':False}
keys=('machineCode','bootId','stateDir','stateSha256','approvalSha256','activeSha256',
      'guardSha256','configSha256','markerSha256','readers','runtimeProcesses','segments','offlineLock')
child=None;work_identity=None;remaining=[]
def save():
    with (root/'result.json').open('w') as out:
        json.dump(result,out,sort_keys=True,indent=2);out.write('\n');out.flush();os.fsync(out.fileno())
def members(pgid):
    rows=[]
    for proc in Path('/proc').iterdir():
        if not proc.name.isdigit():continue
        try:
            raw=(proc/'stat').read_text();fields=raw.rsplit(')',1)[1].split()
            if int(fields[2])==pgid:
                assert int(fields[3])==pgid,'unexpected fixture session'
                rows.append({'pid':int(proc.name),'state':fields[0]})
        except (FileNotFoundError,ProcessLookupError):pass
    return rows
def reap(pgid):
    while True:
        try:
            pid,_=os.waitpid(-pgid,os.WNOHANG)
            if pid==0:return
        except ChildProcessError:return
def terminate_group():
    if child is None:return []
    for sig in (signal.SIGTERM,signal.SIGKILL):
        if not members(child.pid):break
        try:os.killpg(child.pid,sig)
        except ProcessLookupError:pass
        end=time.monotonic()+5
        while time.monotonic()<end:
            child.poll();reap(child.pid)
            if not members(child.pid):break
            time.sleep(0.05)
    child.wait(timeout=2);reap(child.pid)
    return members(child.pid)
def terminated(signum,frame):raise TimeoutError('outer execution deadline/signal')
signal.signal(signal.SIGTERM,terminated)
try:
    assert root.resolve()==root and root.parent==Path('/tmp') and not root.is_symlink()
    verified=[]
    assert {str(p.relative_to(source)) for p in source.rglob('*') if not p.is_dir()}==set(PINS)
    for name,pin in PINS.items():
        p=source/name;s=p.lstat()
        assert not any(v.is_symlink() for v in (p,)+tuple(p.parents))
        assert stat.S_ISREG(s.st_mode) and s.st_uid==0 and stat.S_IMODE(s.st_mode)==0o444
        assert s.st_size==pin['bytes'] and hashlib.sha256(p.read_bytes()).hexdigest()==pin['sha256']
        verified.append({'path':name,**pin})
    result['verifiedInputs']=verified
    result['before']=baseline()
    assert all(result['before']['snapshot'][k]==EXPECTED['snapshot'][k] for k in keys),'preflight drift'
    assert os.statvfs('/tmp').f_bavail*os.statvfs('/tmp').f_frsize>=1024**3
    assert not (work.exists() or work.is_symlink());work.mkdir(mode=0o700)
    ws=work.stat();work_identity=(ws.st_dev,ws.st_ino)
    # Adopt orphaned fixture forks so the group is positively reaped, not just signalled.
    assert ctypes.CDLL(None,use_errno=True).prctl(36,1,0,0,0)==0,'subreaper unavailable'
    def limits():
        for key,value in ((resource.RLIMIT_AS,384*1024**2),(resource.RLIMIT_CPU,60),
                          (resource.RLIMIT_FSIZE,64*1024**2),(resource.RLIMIT_NOFILE,128),
                          (resource.RLIMIT_CORE,0)):resource.setrlimit(key,(value,value))
    leaf="""import json,sys,unittest
from pathlib import Path
sys.path.insert(0,sys.argv[1])
import production_runtime_baseline_test as fixture
names=json.loads(sys.argv[2])
suite=unittest.TestSuite(fixture.ProductionRuntimeTest(name) for name in names)
out=unittest.TextTestRunner(verbosity=2).run(suite)
record=dict(tests=out.testsRun,failures=len(out.failures),errors=len(out.errors),skipped=len(out.skipped))
Path('test-summary.json').write_text(json.dumps(record)+'\\n')
raise SystemExit(0 if out.wasSuccessful() and out.testsRun==8 and not out.skipped else 1)
"""
    command=['/usr/bin/python3','-I','-B','-c',leaf,str(source/'tools'),json.dumps(CASES)]
    env={'PATH':'/usr/bin:/bin','LC_ALL':'C','TMPDIR':str(work)}
    result.update(cases=CASES,fixtureUid=os.geteuid(),fixtureCommand=command,timeoutSeconds=120)
    assert os.geteuid()==0,'private ownership fixture requires root'
    started=time.monotonic()
    with (root/'fixture.log').open('xb') as log:
        child=subprocess.Popen(command,cwd=str(work),env=env,stdout=log,stderr=subprocess.STDOUT,
                               start_new_session=True,preexec_fn=limits)
        result['fixtureProcessGroup']=child.pid;save()
        try:result['fixtureExitCode']=child.wait(timeout=120)
        except subprocess.TimeoutExpired:result['timedOut']=True
    result['fixtureElapsedSeconds']=time.monotonic()-started
except BaseException as error:
    result.update(errorType=type(error).__name__,errorMessage=str(error))
finally:
    try:remaining=terminate_group();result['remainingFixtureProcesses']=remaining
    except BaseException as error:result['groupCleanupError']=str(error);remaining=['unverified']
    try:
        result['after']=baseline()
        before=result.get('before',EXPECTED)['snapshot']
        result['baselineChanges']=[k for k in keys if before[k]!=result['after']['snapshot'][k]]
    except BaseException as error:result['postcheckError']=str(error)
    log=root/'fixture.log'
    if log.exists():
        assert log.stat().st_size<=131072,'fixture log output bound exceeded'
        raw=log.read_bytes();result.update(fixtureLog=raw.decode('utf-8','replace'),
            fixtureLogSha256=hashlib.sha256(raw).hexdigest(),fixtureLogBytes=len(raw))
    summary=work/'test-summary.json'
    if summary.exists():result['testSummary']=json.loads(summary.read_text())
    # Retain source, logs, result, and both raw baselines. Only our initially-empty
    # work directory can be removed, after group absence and unchanged business proof.
    if work_identity and not remaining and result.get('baselineChanges')==[] and not result.get('postcheckError'):
        s=work.lstat()
        assert work.resolve()==work and not work.is_symlink() and (s.st_dev,s.st_ino)==work_identity
        assert shutil.rmtree.avoids_symlink_attacks
        shutil.rmtree(str(work));result['workRemoved']=not work.exists()
    save()
print(json.dumps(result))
raise SystemExit(0 if result.get('fixtureExitCode')==0 and result.get('testSummary')==
    {'tests':8,'failures':0,'errors':0,'skipped':0} and result.get('baselineChanges')==[] and
    result.get('remainingFixtureProcesses')==[] and result.get('workRemoved') and
    not any(k in result for k in ('errorType','postcheckError','groupCleanupError','timedOut')) else 1)
'''


def blob(path):
    return subprocess.check_output(['git','cat-file','blob',COMMIT+':'+path],cwd=str(REPO))


def invoke(client, code, seconds):
    i,o,e=client.exec_command('timeout --signal=TERM --kill-after=20s %ds python3 -I -B -' % seconds,
                              timeout=seconds+30)
    i.write(code);i.channel.shutdown_write()
    raw,err=o.read(524289),e.read(16385);status=o.channel.recv_exit_status()
    assert len(raw)<=524288 and len(err)<=16384,'remote output exceeds cap'
    return status,raw,err


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--execute-approved',action='store_true')
    args=parser.parse_args()
    raw=blob('evidence/20261001-production-baseline/local-checkpoint.json')
    assert hashlib.sha256(raw).hexdigest()==MANIFEST_SHA
    manifest=json.loads(raw);pins={**manifest['changedInputs'],**manifest['unchangedFixtureDependencies']}
    pins={name:pins[name] for name in FILES};blobs={name:blob(name) for name in FILES}
    for name,data in blobs.items():assert len(data)==pins[name]['bytes'] and hashlib.sha256(data).hexdigest()==pins[name]['sha256']
    probe=(COORD/'d2_device_readonly_probe.py').read_bytes()
    helper=(COORD/'run_bounded_probe.py').read_bytes()
    assert hashlib.sha256(probe).hexdigest()==PROBE_SHA and hashlib.sha256(helper).hexdigest()==CONNECT_SHA
    plan={'host':HOST,'root':ROOT,'commit':COMMIT,'inputs':pins,'cases':CASES,'executeApprovedFlag':args.execute_approved}
    if not args.execute_approved:
        print(json.dumps(plan,indent=2));return 0  # no helper import or connection
    output=HERE/'board-23-r1-result.json'
    with output.open('x',encoding='utf-8') as f:f.write('{"phase":"LOCAL_RESERVED","sshStarted":false}\n')
    result=dict(plan,sshStarted=False);client=None
    try:
        spec=importlib.util.spec_from_file_location('production_existing_connect',COORD/'run_bounded_probe.py')
        module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
        client=module.connect(HOST);result['sshStarted']=True
        prefix='PROBE='+repr(probe.decode())+'\n'+BASELINE_CODE
        code,stdout,stderr=invoke(client,prefix+'\nprint(json.dumps(baseline()))\n',60)
        result.update(preflightExitCode=code,preflightStdout=stdout.decode(),preflightStderr=stderr.decode())
        assert code==0 and not stderr;expected=json.loads(stdout)
        with client.open_sftp() as s:
            info=s.lstat('/tmp');assert stat.S_ISDIR(info.st_mode)
            try:s.lstat(ROOT)
            except FileNotFoundError:pass
            else:raise RuntimeError('occupied unique namespace; refuse retry')
            s.mkdir(ROOT,mode=0o700);result['uploadStarted']=True
            directories={ROOT}
            for name,data in sorted(blobs.items()):
                destination=ROOT+'/source/'+name;parts=destination.split('/')
                for n in range(3,len(parts)):
                    directory='/'.join(parts[:n])
                    if directory not in directories:s.mkdir(directory,mode=0o700);directories.add(directory)
                with s.open(destination,'wx') as f:f.write(data)
                s.chmod(destination,0o444)
            for directory in sorted(directories-{ROOT},key=len,reverse=True):s.chmod(directory,0o500)
        params=dict(HOST=HOST,ROOT=ROOT,COMMIT=COMMIT,PINS=pins,CASES=CASES,EXPECTED=expected)
        code,stdout,stderr=invoke(client,'globals().update('+repr(params)+')\n'+prefix+RUN_CODE,210)
        result.update(remoteExitCode=code,remoteStdout=stdout.decode(),remoteStderr=stderr.decode())
        if stdout:result['execution']=json.loads(stdout)
    except BaseException as error:result.update(errorType=type(error).__name__,errorMessage=str(error))
    finally:
        if client is not None:client.close()
        result['sshClosed']=True
        output.write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print(json.dumps({k:v for k,v in result.items() if k not in ('execution','remoteStdout','preflightStdout')}))
    return 0 if result.get('remoteExitCode')==0 and not result.get('remoteStderr') and 'errorType' not in result else 1


if __name__=='__main__':raise SystemExit(main())
