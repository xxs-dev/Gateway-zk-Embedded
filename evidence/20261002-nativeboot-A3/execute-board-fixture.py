"""Review-only until --execute-approved; 23-only private Python fixture, no host lifecycle."""
import argparse
import ast
import hashlib
import importlib.util
import json
from pathlib import Path
import stat
import subprocess

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
COORD = Path('D:/workspace/GatewaySuite-workspaces/realese1.0/coordinator/docs/audits/evidence/20261001-device-acceptance')
COMMIT = '73e2f9f627070ebb4ac03e41c0a50ce6b2032761'
HOST = '192.168.22.23'
ROOT = '/tmp/gw-nativeboot-73e2f9f-23-A3'
PROBE_SHA = '509a74fcf5658156773d1273b4e2222d5b8d687da57bc8cdaf0086a88765a221'
CONNECT_SHA = 'd8dfd569062fee7bbb022c6f4f49b93bdd4a0415a819d1c2e5bfcb16f2dc7ccf'
MANIFEST_SHA = '9429dcce310cb6f52f1eda5558a864d7d99a38597ee01481a61d48e37736d559'
FILES = (
    'deploy/offline-runtime-upgrade.py',
    'deploy/runtime-upgrade-guard.py',
    'deploy/ota-health-result.py',
    'tools/production_runtime_baseline_test.py',
    'tools/production_boot_publication_test.py',
    'tools/production_native_boot_test.py',
    'tools/production_boot_health_publication_test.py',
    'deploy/gateway-services.sh',
    'deploy/ota-rollback.sh',
    'deploy/gateway-ota-recover.service',
    'tools/fixtures/production-historical-r2-allowlists.json',
)
CASES = (
    'production_native_boot_test.NativeBootTest.test_cleared_run_real_entry_creates_locked_boot_proof_before_readers',
    'production_native_boot_test.NativeBootTest.test_interrupted_creator_reenters_original_intent_and_inode',
    'production_native_boot_test.NativeBootTest.test_wrong_interrupted_header_is_not_truncated_or_replaced',
    'production_native_boot_test.NativeBootTest.test_original_job_newboot_pending',
    'production_native_boot_test.NativeBootTest.test_original_job_newboot_promoted',
    'production_native_boot_test.NativeBootTest.test_original_job_newboot_rollback',
    'production_native_boot_test.NativeBootTest.test_original_job_newboot_preintent',
    'production_native_boot_test.NativeBootTest.test_postgate_worker_refuses_while_recovery_oneshot_activating',
    'production_native_boot_test.NativeBootTest.test_reader_start_can_require_gate_without_deadlock_or_worker_replay',
    'production_native_boot_test.NativeBootTest.test_failed_newboot_health_refences_and_official_worker_can_recover',
    'production_native_boot_test.NativeBootTest.test_health_published_then_directory_sync_failure_retry_binds_new_pid_attempt',
    'production_native_boot_test.NativeBootTest.test_no_handoff_runtime_newboot_promoted',
    'production_native_boot_test.NativeBootTest.test_no_handoff_runtime_newboot_adoption_rollback',
    'production_native_boot_test.NativeBootTest.test_uncertain_dispatch_original_finish_recovers_without_gate_resend',
    'production_native_boot_test.NativeBootTest.test_memory_or_tmpfs_insufficient_refuses_before_intent_or_native_call',
    'production_native_boot_test.NativeBootTest.test_retired_recreated_or_unowned_target_refuses',
    'production_native_boot_test.NativeBootTest.test_invalid_stats_never_publishes_ready_or_admits_reader',
    'production_native_boot_test.NativeBootTest.test_not_tmpfs_or_held_lock_refuses_before_native_call',
    'production_boot_publication_test.BootQuiescenceTest.test_approved_missing_new_boot_roles_scan_without_mutation',
    'production_boot_publication_test.BootQuiescenceTest.test_same_boot_and_legacy_missing_roles_remain_strict',
    'production_boot_publication_test.BootQuiescenceTest.test_unowned_target_and_recreated_retired_source_refuse',
    'production_boot_publication_test.BootQuiescenceTest.test_owned_progress_and_complete_roles_validate_identity_and_abi',
    'production_boot_publication_test.BootQuiescenceTest.test_existing_symlink_hardlink_and_wrong_mode_fail_closed',
    'production_boot_publication_test.BootQuiescenceTest.test_deleted_target_and_retired_name_mappings_refuse_when_paths_absent',
    'production_boot_publication_test.BootQuiescenceTest.test_deleted_fd_name_refuses_even_if_fd_stat_would_fail',
    'production_boot_publication_test.BootQuiescenceTest.test_alias_mapping_is_rejected_by_actual_identity',
    'production_boot_publication_test.BootQuiescenceTest.test_unrelated_new_object_reusing_historical_tuple_is_not_a_holder',
    'production_boot_publication_test.BootQuiescenceTest.test_permission_unknown_visibility_and_malformed_maps_fail_closed',
    'production_boot_publication_test.BootQuiescenceTest.test_lock_and_approval_required_and_completed_role_cannot_vanish',
    'production_boot_publication_test.BootLockTest.test_same_boot_entry_missing_lock_succeeds_without_service_or_shm_mutation',
    'production_runtime_baseline_test.ProductionRuntimeTest.test_boot_hook_resolves_durable_owner_tail_even_without_runtime_pending',
)
BASELINE_CODE = r'''
import contextlib,io,json,stat,subprocess
from pathlib import Path
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
    identities=[]
    for segment in value['segments']:
        for role in ('source','target'):
            path=Path('/dev/shm')/segment[role]
            row={'role':role,'name':segment[role],'present':path.exists() or path.is_symlink()}
            if row['present']:
                info=path.lstat()
                assert stat.S_ISREG(info.st_mode) and not path.is_symlink()
                with path.open('rb') as stream:header=stream.read(8).hex()
                row.update(dev=info.st_dev,ino=info.st_ino,bytes=info.st_size,mode=stat.S_IMODE(info.st_mode),header=header)
            identities.append(row)
    value['segmentIdentities']=identities
    ticks={}
    maps={}
    for reader in value['readers']:
        pid=reader['properties']['MainPID'];proc=Path('/proc')/pid
        fields=(proc/'stat').read_text().rsplit(')',1)[1].split()
        ticks[pid]=fields[19]
        with (proc/'maps').open() as stream:rawmaps=stream.read(131073)
        assert len(rawmaps)<=131072
        maps[pid]=[line for line in rawmaps.splitlines() if '/dev/shm/' in line]
    value['readerStartTicks']=ticks;value['readerShmMaps']=maps
    return {'snapshot':value,'rawJson':raw,'rawCommands':raw_commands}
'''
RUN_CODE = r'''
import ctypes,hashlib,os,resource,shutil,signal,stat,time
from pathlib import Path
root=Path(ROOT);source=root/'source';work=root/'work'
result={'host':HOST,'commit':COMMIT,'root':ROOT,'fixtureOnly':True,'realLifecycleCalled':False}
keys=('machineCode','bootId','stateDir','stateSha256','approvalSha256','activeSha256',
      'guardSha256','configSha256','markerSha256','readers','runtimeProcesses','segments','offlineLock','segmentIdentities','readerStartTicks','readerShmMaps')
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
        for key,value in ((resource.RLIMIT_AS,384*1024**2),(resource.RLIMIT_CPU,180),
                          (resource.RLIMIT_FSIZE,64*1024**2),(resource.RLIMIT_NOFILE,128),
                          (resource.RLIMIT_CORE,0)):resource.setrlimit(key,(value,value))
    leaf="""import json,sys,unittest
from pathlib import Path
sys.path.insert(0,sys.argv[1])
names=json.loads(sys.argv[2])
suite=unittest.defaultTestLoader.loadTestsFromNames(names)
out=unittest.TextTestRunner(verbosity=2).run(suite)
record=dict(tests=out.testsRun,failures=len(out.failures),errors=len(out.errors),skipped=len(out.skipped))
Path('test-summary.json').write_text(json.dumps(record)+'\\n')
raise SystemExit(0 if out.wasSuccessful() and out.testsRun==31 and not out.skipped else 1)
"""
    command=['/usr/bin/python3','-I','-B','-c',leaf,str(source/'tools'),json.dumps(CASES)]
    env={'PATH':'/usr/bin:/bin','LC_ALL':'C','TMPDIR':str(work)}
    result.update(cases=CASES,fixtureUid=os.geteuid(),fixtureCommand=command,timeoutSeconds=240)
    assert os.geteuid()==0,'private ownership fixture requires root'
    started=time.monotonic()
    with (root/'fixture.log').open('xb') as log:
        child=subprocess.Popen(command,cwd=str(work),env=env,stdout=log,stderr=subprocess.STDOUT,
                               start_new_session=True,preexec_fn=limits)
        result['fixtureProcessGroup']=child.pid;save()
        try:result['fixtureExitCode']=child.wait(timeout=240)
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
        assert log.stat().st_size<=262144,'fixture log output bound exceeded'
        raw=log.read_bytes();result.update(fixtureLog=raw.decode('utf-8','replace'),
            fixtureLogSha256=hashlib.sha256(raw).hexdigest(),fixtureLogBytes=len(raw))
    summary=work/'test-summary.json'
    if summary.exists():result['testSummary']=json.loads(summary.read_text())
    # Retain source, logs, result, and both raw baselines. Only our initially-empty
    # work directory can be removed, after group absence and unchanged business proof.
    if work_identity and not remaining and result.get('fixtureExitCode')==0 and result.get('testSummary')=={'tests':31,'failures':0,'errors':0,'skipped':0} and result.get('baselineChanges')==[] and not result.get('postcheckError'):
        s=work.lstat()
        assert work.resolve()==work and not work.is_symlink() and (s.st_dev,s.st_ino)==work_identity
        assert shutil.rmtree.avoids_symlink_attacks
        shutil.rmtree(str(work));result['workRemoved']=not work.exists()
    # Final acceptance is based on the baseline AFTER owned-work cleanup.
    try:
        result['afterCleanup']=baseline()
        before=result.get('before',EXPECTED)['snapshot']
        result['cleanupBaselineChanges']=[k for k in keys if before[k]!=result['afterCleanup']['snapshot'][k]]
        result['postInputHashes']={}
        for name,pin in PINS.items():
            info=(source/name).lstat()
            assert stat.S_ISREG(info.st_mode) and stat.S_IMODE(info.st_mode)==0o444
            result['postInputHashes'][name]=hashlib.sha256((source/name).read_bytes()).hexdigest()
            assert result['postInputHashes'][name]==pin['sha256']
    except BaseException as error:result['postCleanupError']=str(error)
    save()
print(json.dumps(result))
raise SystemExit(0 if result.get('fixtureExitCode')==0 and result.get('testSummary')==
    {'tests':31,'failures':0,'errors':0,'skipped':0} and result.get('baselineChanges')==[] and
    result.get('remainingFixtureProcesses')==[] and result.get('workRemoved') and
    result.get('cleanupBaselineChanges')==[] and 'postCleanupError' not in result and
    not any(k in result for k in ('errorType','postcheckError','groupCleanupError','timedOut')) else 1)
'''


def blob(path):
    return subprocess.check_output(['git','cat-file','blob',COMMIT+':'+path],cwd=str(REPO))


def invoke(client, code, seconds):
    i,o,e=client.exec_command('timeout --signal=TERM --kill-after=20s %ds python3 -I -B -' % seconds,
                              timeout=seconds+30)
    i.write(code);i.channel.shutdown_write()
    raw,err=o.read(1048577),e.read(16385);status=o.channel.recv_exit_status()
    assert len(raw)<=1048576 and len(err)<=16384,'remote output exceeds cap'
    return status,raw,err


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--execute-approved',action='store_true')
    args=parser.parse_args()
    raw=blob('evidence/20261002-nativeboot-A3/local-checkpoint.json')
    assert hashlib.sha256(raw).hexdigest()==MANIFEST_SHA
    manifest=json.loads(raw);pins={row['path']:{'bytes':row['bytes'],'sha256':row['sha256']} for row in manifest['files']}
    pins={name:pins[name] for name in FILES};blobs={name:blob(name) for name in FILES}
    for name,data in blobs.items():assert len(data)==pins[name]['bytes'] and hashlib.sha256(data).hexdigest()==pins[name]['sha256']
    assert len(FILES)==11 and len(CASES)==31 and len(set(CASES))==31
    assert list(CASES)==manifest['cases'],'selector/manifest drift'
    for case in CASES:
        module,cls,method=case.split('.')
        tree=ast.parse(blobs['tools/'+module+'.py'],feature_version=(3,8))
        assert any(isinstance(n,ast.ClassDef) and n.name==cls and
                   any(isinstance(v,ast.FunctionDef) and v.name==method for v in n.body) for n in tree.body),case
    ast.parse(BASELINE_CODE,feature_version=(3,8))
    remote=ast.parse(RUN_CODE,feature_version=(3,8))
    leaf=next(n.value.value for n in ast.walk(remote) if isinstance(n,ast.Assign) and
              any(isinstance(t,ast.Name) and t.id=='leaf' for t in n.targets))
    ast.parse(leaf,feature_version=(3,8))
    probe=(COORD/'d2_device_readonly_probe.py').read_bytes()
    helper=(COORD/'run_bounded_probe.py').read_bytes()
    assert hashlib.sha256(probe).hexdigest()==PROBE_SHA and hashlib.sha256(helper).hexdigest()==CONNECT_SHA
    plan={'host':HOST,'root':ROOT,'commit':COMMIT,'inputs':pins,'cases':CASES,'executeApprovedFlag':args.execute_approved,'remoteAuthorized':False,'fixtureOnly':True,'selectedTestCount':31}
    if not args.execute_approved:
        print(json.dumps(plan,indent=2));return 0  # no helper import or connection
    output=HERE/'board-23-A3-result.json'
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
        code,stdout,stderr=invoke(client,'globals().update('+repr(params)+')\n'+prefix+RUN_CODE,300)
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
