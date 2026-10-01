"""Review-only until --execute-approved: 23-only actual native CLI, child private mounts."""
import argparse
import ast
import hashlib
import importlib.util
import json
from pathlib import Path
import stat
import struct
import tarfile
import subprocess

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
COORD = Path('D:/workspace/GatewaySuite-workspaces/realese1.0/coordinator/docs/audits/evidence/20261001-device-acceptance')
COMMIT = '1950402a5d1c97e661d7132eae065508f32ab194'
HOST = '192.168.22.23'
ROOT = '/tmp/gw-nativeboot-f9a79bd-23-B1'
PROBE_SHA = '509a74fcf5658156773d1273b4e2222d5b8d687da57bc8cdaf0086a88765a221'
CONNECT_SHA = 'd8dfd569062fee7bbb022c6f4f49b93bdd4a0415a819d1c2e5bfcb16f2dc7ccf'
MANIFEST_SHA = '844716aeb6c9426d98b6649e844c8947d213b96423064c2e5a6fd986a5a06bed'
FILES = ('native-child.py', 'pointctl')
NAMES = ('gw_native_b1_f9a79bd_cold', 'gw_native_b1_f9a79bd_interrupt')
CHECKS = (
    'cold-create',
    'reopen-cold',
    'interrupt-after-ftruncate',
    'resume-interrupted-zero-header',
    'reopen-resumed',
    'reject-explicit-private-short-size',
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
# A4's bounded business sampler above is unchanged. B adds parent namespace proof.
business_baseline = baseline
def baseline():
    import hashlib,os
    value=business_baseline()
    with Path('/proc/self/mountinfo').open() as stream:raw=stream.read(262145)
    assert len(raw)<=262144
    directory=Path('/dev/shm').stat()
    names={name:os.path.lexists('/dev/shm/'+name) for name in NAMES}
    assert not any(names.values()),'B private role already exists in parent namespace'
    namespaces={reader['properties']['MainPID']:os.readlink('/proc/'+reader['properties']['MainPID']+'/ns/mnt')
                for reader in value['snapshot']['readers']}
    value['parentNamespace']={'namespace':os.readlink('/proc/self/ns/mnt'),
        'shmIdentity':[directory.st_dev,directory.st_ino],'mountinfoRaw':raw,
        'mountinfoSha256':hashlib.sha256(raw.encode()).hexdigest(),
        'readerNamespaces':namespaces,'parentNativeNames':names}
    return value

'''
RUN_CODE = r'''
import ctypes,hashlib,os,resource,shutil,signal,stat,time
from pathlib import Path
root=Path(ROOT);source=root/'source';work=root/'work'
result={'host':HOST,'commit':COMMIT,'root':ROOT,'privateNativeOnly':True,'realLifecycleCalled':False}
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
        assert stat.S_ISREG(s.st_mode) and s.st_uid==0 and stat.S_IMODE(s.st_mode)==pin['mode']
        assert s.st_size==pin['bytes'] and hashlib.sha256(p.read_bytes()).hexdigest()==pin['sha256']
        verified.append({'path':name,**pin})
    result['verifiedInputs']=verified
    result['before']=baseline()
    assert all(result['before']['snapshot'][k]==EXPECTED['snapshot'][k] for k in keys) and result['before']['parentNamespace']==EXPECTED['parentNamespace'],'preflight drift'
    assert os.statvfs('/tmp').f_bavail*os.statvfs('/tmp').f_frsize>=1024**3
    assert not (work.exists() or work.is_symlink());work.mkdir(mode=0o700)
    ws=work.stat();work_identity=(ws.st_dev,ws.st_ino)
    # Adopt orphaned fixture forks so the group is positively reaped, not just signalled.
    assert ctypes.CDLL(None,use_errno=True).prctl(36,1,0,0,0)==0,'subreaper unavailable'
    def limits():
        for key,value in ((resource.RLIMIT_AS,384*1024**2),(resource.RLIMIT_CPU,180),
                          (resource.RLIMIT_FSIZE,64*1024**2),(resource.RLIMIT_NOFILE,128),
                          (resource.RLIMIT_CORE,0)):resource.setrlimit(key,(value,value))
    command=['/usr/bin/python3','-I','-B',str(source/'native-child.py'),str(root),
             json.dumps(result['before']['parentNamespace'])]
    env={'PATH':'/usr/bin:/bin','LC_ALL':'C','TMPDIR':str(work)}
    result.update(checks=CHECKS,fixtureUid=os.geteuid(),fixtureCommand=command,timeoutSeconds=180)
    assert os.geteuid()==0,'private native validation requires root'
    started=time.monotonic()
    with (root/'fixture.log').open('xb') as log:
        child=subprocess.Popen(command,cwd=str(work),env=env,stdout=log,stderr=subprocess.STDOUT,
                               start_new_session=True,preexec_fn=limits)
        result['fixtureProcessGroup']=child.pid;save()
        try:result['fixtureExitCode']=child.wait(timeout=180)
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
        if result['after']['parentNamespace']!=result.get('before',EXPECTED)['parentNamespace']:
            result['baselineChanges'].append('parentNamespace')
    except BaseException as error:result['postcheckError']=str(error)
    log=root/'fixture.log'
    if log.exists():
        assert log.stat().st_size<=262144,'fixture log output bound exceeded'
        raw=log.read_bytes();result.update(fixtureLog=raw.decode('utf-8','replace'),
            fixtureLogSha256=hashlib.sha256(raw).hexdigest(),fixtureLogBytes=len(raw))
    summary=root/'native-result.json'
    try:
        if summary.exists() or summary.is_symlink():
            info=summary.lstat()
            assert stat.S_ISREG(info.st_mode) and not summary.is_symlink() and info.st_uid==0 and info.st_nlink==1
            assert info.st_size<=262144
            native=json.loads(summary.read_text())
            result['nativeResult']=native
            result['nativeCheckSummary']={'checks':len(native['checks']),
                                         'passed':sum(row['passed'] is True for row in native['checks'])}
            result['nativeChecksPassed']=native.get('passed') is True and [row['case'] for row in native['checks']]==list(CHECKS)
    except BaseException as error:result['nativeResultError']=str(error)
    # Retain source, logs, result, and both raw baselines. Only our initially-empty
    # work directory can be removed, after group absence and unchanged business proof.
    if work_identity and not remaining and result.get('fixtureExitCode')==0 and result.get('nativeChecksPassed') and result.get('nativeCheckSummary')=={'checks':6,'passed':6} and result.get('baselineChanges')==[] and not result.get('postcheckError'):
        s=work.lstat()
        assert work.resolve()==work and not work.is_symlink() and (s.st_dev,s.st_ino)==work_identity
        assert shutil.rmtree.avoids_symlink_attacks
        shutil.rmtree(str(work));result['workRemoved']=not work.exists()
    # Final acceptance is based on the baseline AFTER owned-work cleanup.
    try:
        result['afterCleanup']=baseline()
        before=result.get('before',EXPECTED)['snapshot']
        result['cleanupBaselineChanges']=[k for k in keys if before[k]!=result['afterCleanup']['snapshot'][k]]
        if result['afterCleanup']['parentNamespace']!=result.get('before',EXPECTED)['parentNamespace']:
            result['cleanupBaselineChanges'].append('parentNamespace')
        result['postInputHashes']={}
        for name,pin in PINS.items():
            info=(source/name).lstat()
            assert stat.S_ISREG(info.st_mode) and stat.S_IMODE(info.st_mode)==pin['mode']
            result['postInputHashes'][name]=hashlib.sha256((source/name).read_bytes()).hexdigest()
            assert result['postInputHashes'][name]==pin['sha256']
    except BaseException as error:result['postCleanupError']=str(error)
    save()
print(json.dumps(result))
raise SystemExit(0 if result.get('fixtureExitCode')==0 and result.get('nativeChecksPassed') and result.get('nativeCheckSummary')==
    {'checks':6,'passed':6} and result.get('baselineChanges')==[] and
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
    raw=blob('evidence/20261002-nativeboot-B1/local-checkpoint.json')
    assert hashlib.sha256(raw).hexdigest()==MANIFEST_SHA
    manifest=json.loads(raw)
    assert manifest['root']==ROOT and manifest['names']==list(NAMES) and manifest['checks']==list(CHECKS)
    pins={row['sourceName']:{'bytes':row['bytes'],'sha256':row['sha256'],'mode':row['mode']} for row in manifest['files']}
    blobs={'native-child.py':blob(manifest['files'][0]['path'])}
    package=Path(manifest['artifact']['packagePath'])
    assert package.stat().st_size==manifest['artifact']['packageBytes']
    value=hashlib.sha256()
    with package.open('rb') as stream:
        for data in iter(lambda:stream.read(65536),b''):value.update(data)
    assert value.hexdigest()==manifest['artifact']['packageSha256']
    with tarfile.open(package,'r:gz') as archive:
        members=archive.getmembers();assert len(members)<=64
        matches=[row for row in members if row.name==manifest['artifact']['member']]
        assert len(matches)==1 and matches[0].isreg() and matches[0].size==pins['pointctl']['bytes']
        blobs['pointctl']=archive.extractfile(matches[0]).read(pins['pointctl']['bytes']+1)
    assert set(blobs)==set(FILES) and len(CHECKS)==6 and len(NAMES)==2
    for name,data in blobs.items():
        assert len(data)==pins[name]['bytes'] and hashlib.sha256(data).hexdigest()==pins[name]['sha256']
    elf=blobs['pointctl']
    assert elf[:6]==b'\x7fELF\x02\x01' and struct.unpack_from('<H',elf,18)[0]==183
    for row in manifest['sourceAudit']:
        actual=subprocess.check_output(['git','rev-parse',COMMIT+':'+row['path']],cwd=str(REPO)).decode().strip()
        assert actual==row['currentBlob']==row['historicalBlob']
    ast.parse(blobs['native-child.py'],feature_version=(3,8))
    ast.parse(BASELINE_CODE,feature_version=(3,8));ast.parse(RUN_CODE,feature_version=(3,8))
    probe=(COORD/'d2_device_readonly_probe.py').read_bytes()
    helper=(COORD/'run_bounded_probe.py').read_bytes()
    assert hashlib.sha256(probe).hexdigest()==PROBE_SHA and hashlib.sha256(helper).hexdigest()==CONNECT_SHA
    plan={'host':HOST,'root':ROOT,'commit':COMMIT,'inputs':pins,'checks':CHECKS,'names':NAMES,
          'executeApprovedFlag':args.execute_approved,'remoteAuthorized':False,'privateNativeOnly':True,
          'plannedNativeCommands':6,'sourceAuditCount':11}
    if not args.execute_approved:
        print(json.dumps(plan,indent=2));return 0  # No helper import, SSH or native execution.
    output=HERE/'board-23-B1-result.json'
    with output.open('x',encoding='utf-8') as f:f.write('{"phase":"LOCAL_RESERVED","sshStarted":false}\n')
    result=dict(plan,sshStarted=False);client=None
    try:
        spec=importlib.util.spec_from_file_location('production_existing_connect',COORD/'run_bounded_probe.py')
        module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
        client=module.connect(HOST);result['sshStarted']=True
        prefix='PROBE='+repr(probe.decode())+'\nNAMES='+repr(NAMES)+'\n'+BASELINE_CODE
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
                s.chmod(destination,pins[name]['mode'])
            for directory in sorted(directories-{ROOT},key=len,reverse=True):s.chmod(directory,0o500)
        params=dict(HOST=HOST,ROOT=ROOT,COMMIT=COMMIT,PINS=pins,CHECKS=CHECKS,NAMES=NAMES,EXPECTED=expected)
        code,stdout,stderr=invoke(client,'globals().update('+repr(params)+')\n'+prefix+RUN_CODE,240)
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
