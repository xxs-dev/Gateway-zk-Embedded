"""Single read-only evidence export, descriptor-relative O_NOFOLLOW at every level."""
import hashlib
import importlib.util
import json
from pathlib import Path
import io
import tarfile
import time

HERE = Path(__file__).resolve().parent
ROOT = '/tmp/gw-nativeboot-082071e-23-C1'
HELPER = Path('D:/workspace/GatewaySuite-workspaces/realese1.0/coordinator/docs/audits/evidence/20261001-device-acceptance/run_bounded_probe.py')

EXPORT = r'''
import hashlib,io,json,os,signal,stat,sys,tarfile,time,resource
root='/tmp/gw-nativeboot-082071e-23-C1'
def expired(signum,frame):raise TimeoutError('evidence export deadline')
signal.signal(signal.SIGALRM,expired);signal.alarm(90)
for key,value in ((resource.RLIMIT_CPU,30),(resource.RLIMIT_AS,256*1024**2),(resource.RLIMIT_NOFILE,128),(resource.RLIMIT_CORE,0)):
    resource.setrlimit(key,(value,value))
index=dict(root=root,files=[],totalBytes=0,directories=0,remoteWrites=0)
def identity(s):return (s.st_dev,s.st_ino,s.st_mode,s.st_uid,s.st_nlink,s.st_size,s.st_mtime_ns,s.st_ctime_ns)
flags=os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW|os.O_CLOEXEC
tmp=os.open('/tmp',flags)
fd=os.open(root.rsplit('/',1)[1],flags,dir_fd=tmp);os.close(tmp)
assert os.fstat(fd).st_uid==0
with tarfile.open(fileobj=sys.stdout.buffer,mode='w|') as bundle:
    def walk(directory,prefix):
        index['directories']+=1;assert index['directories']<=128
        for name in sorted(os.listdir(directory)):
            assert name not in ('.','..') and '/' not in name
            relative=prefix+name;s=os.stat(name,dir_fd=directory,follow_symlinks=False)
            assert s.st_uid==0 and not stat.S_ISLNK(s.st_mode)
            if stat.S_ISDIR(s.st_mode):
                child=os.open(name,flags,dir_fd=directory)
                try:
                    assert identity(os.fstat(child))==identity(s)
                    walk(child,relative+'/')
                finally:os.close(child)
                continue
            assert stat.S_ISREG(s.st_mode) and s.st_nlink==1 and s.st_size<=1024**2
            assert len(index['files'])<512 and index['totalBytes']+s.st_size<=16*1024**2
            file=os.open(name,os.O_RDONLY|os.O_NOFOLLOW|os.O_CLOEXEC,dir_fd=directory)
            try:
                assert identity(os.fstat(file))==identity(s)
                with os.fdopen(file,'rb',closefd=False) as stream:data=stream.read(1024**2+1)
                assert len(data)==s.st_size and identity(os.fstat(file))==identity(s)
                assert identity(os.stat(name,dir_fd=directory,follow_symlinks=False))==identity(s)
            finally:os.close(file)
            row=dict(path=relative,bytes=len(data),sha256=hashlib.sha256(data).hexdigest(),uid=s.st_uid,
                     mode=stat.S_IMODE(s.st_mode),dev=s.st_dev,inode=s.st_ino,mtimeNs=s.st_mtime_ns)
            info=tarfile.TarInfo(relative);info.size=len(data);info.mode=row['mode'];info.uid=0;info.mtime=s.st_mtime
            bundle.addfile(info,io.BytesIO(data));index['files'].append(row);index['totalBytes']+=len(data)
    try:walk(fd,'')
    finally:os.close(fd)
    raw=(json.dumps(index,sort_keys=True)+'\n').encode()
    info=tarfile.TarInfo('EXPORT-INDEX.json');info.size=len(raw);info.mode=0o600
    bundle.addfile(info,io.BytesIO(raw))
'''


def main():
    assert hashlib.sha256(HELPER.read_bytes()).hexdigest() == 'd8dfd569062fee7bbb022c6f4f49b93bdd4a0415a819d1c2e5bfcb16f2dc7ccf'
    report = HERE / 'board-23-C1-retrieval.json'
    archive = HERE / 'board-23-C1-raw.tar'
    assert not report.exists() and not archive.exists(), 'single retrieval already reserved'
    with report.open('x') as output: output.write('{"phase":"LOCAL_RESERVED"}\n')
    value = dict(root=ROOT, host='192.168.22.23', files=[], totalBytes=0, remoteWrites=0, sshClosed=False)
    client = None; started = time.monotonic()
    try:
        spec = importlib.util.spec_from_file_location('c1_existing_evidence_connect', HELPER)
        module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
        client = module.connect('192.168.22.23')
        command = 'timeout --signal=TERM --kill-after=5s 110s python3 -I -B -'
        stdin, stdout, stderr = client.exec_command(command, timeout=125)
        stdin.write(EXPORT); stdin.channel.shutdown_write()
        raw = stdout.read(20 * 1024**2 + 1); errors = stderr.read(16385)
        code = stdout.channel.recv_exit_status()
        value.update(command=command, remoteExitCode=code, remoteStderr=errors.decode('utf-8', 'replace'))
        with archive.open('xb') as output: output.write(raw)
        assert code == 0 and not errors and len(raw) <= 20 * 1024**2
        with tarfile.open(fileobj=io.BytesIO(raw), mode='r:') as bundle:
            members = bundle.getmembers()
            assert all(m.isfile() for m in members) and len(members) <= 513
            assert len({m.name for m in members}) == len(members)
            value.update(json.load(bundle.extractfile('EXPORT-INDEX.json')))
            assert len(value['files']) <= 512 and value['totalBytes'] <= 16 * 1024**2
            assert {m.name for m in members} == {r['path'] for r in value['files']} | {'EXPORT-INDEX.json'}
            for row in value['files']:
                data = bundle.extractfile(row['path']).read(1024**2 + 1)
                assert len(data) == row['bytes'] and hashlib.sha256(data).hexdigest() == row['sha256']
        value['passed'] = True
    except BaseException as error:
        value.update(passed=False, errorType=type(error).__name__, errorMessage=str(error))
    finally:
        if client is not None: client.close()
        value['sshClosed'] = True; value['seconds'] = time.monotonic() - started
        if archive.exists():
            value['archive'] = dict(path=archive.name, bytes=archive.stat().st_size,
                                    sha256=hashlib.sha256(archive.read_bytes()).hexdigest())
        report.write_text(json.dumps(value, indent=2) + '\n')
    print(json.dumps({k: v for k, v in value.items() if k != 'files'}))
    return 0 if value.get('passed') else 1


if __name__ == '__main__':
    raise SystemExit(main())
