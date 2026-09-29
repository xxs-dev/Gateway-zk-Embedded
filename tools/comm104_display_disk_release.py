"""Deploy display-only rules and bounded diagnostics; archive legacy outbox explicitly."""
import base64,gzip,hashlib,json,os,shlex,shutil,time
from pathlib import Path
import paramiko

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'artifacts'/('comm104-display-disk-'+time.strftime('%Y%m%d-%H%M%S'))
OUT.mkdir(parents=True)
c=paramiko.SSHClient();c.load_system_host_keys();c.set_missing_host_key_policy(paramiko.RejectPolicy())
c.connect('10.126.126.11',username='root',password=os.environ['EDGE_PASSWORD'],timeout=10,look_for_keys=False,allow_agent=False)
def run(code,timeout=90):
    _,o,e=c.exec_command('python3 -c '+shlex.quote(code),timeout=timeout)
    data=o.read().decode();err=e.read().decode()
    if o.channel.recv_exit_status():raise RuntimeError(err+data)
    return json.loads(data)
header="""
import pathlib,json,os,subprocess,hashlib,time,shutil
root=pathlib.Path('/opt/modbus-gateway')
assert json.loads((root/'config/runtime/device_identity.json').read_text())['machineCode']=='COMM202600104'
def owners(p):
 result=[]
 for proc in pathlib.Path('/proc').glob('[0-9]*'):
  try:
   for fd in (proc/'fd').iterdir():
    if os.readlink(str(fd)).startswith(str(p)):result.append(proc.name)
  except OSError:pass
 return result
"""
try:
    files={'/usr/local/sbin/gateway-disk-guard':ROOT/'deploy/gateway-disk-guard.sh',
           '/etc/systemd/system/gateway-disk-guard.service':ROOT/'deploy/gateway-disk-guard.service',
           '/etc/systemd/system/gateway-disk-guard.timer':ROOT/'deploy/gateway-disk-guard.timer'}
    payload={k:base64.b64encode(v.read_bytes()).decode() for k,v in files.items()}
    helper=base64.b64encode((ROOT/'tools/fix_pcs_status_display.py').read_bytes()).decode()
    release=run(header+f"""
import base64
old=(root/'scada/current').resolve();assert str(old).startswith(str(root/'scada/releases')+'/')
dest=root/'scada/releases'/('comm104-status-'+time.strftime('%Y%m%d-%H%M%S'))
shutil.copytree(old,dest)
scope={{'__name__':'patch_module'}}
exec(base64.b64decode({helper!r}),scope)
changed=scope['patch'](dest)
assert len(changed)==3,changed
checks={{str(p.relative_to(dest)):hashlib.sha256(p.read_bytes()).hexdigest() for p in dest.rglob('*') if p.is_file() and p.name!='checksums.json'}}
(dest/'checksums.json').write_text(json.dumps(checks,indent=2))
link=root/'scada/current.new';link.symlink_to(dest);os.replace(link,root/'scada/current')
try:
 subprocess.run(['systemctl','restart','ky-ems.service'],check=True,timeout=30)
 time.sleep(3)
 subprocess.run(['systemctl','is-active','--quiet','ky-ems.service'],check=True)
except BaseException:
 link.symlink_to(old);os.replace(link,root/'scada/current');subprocess.run(['systemctl','restart','ky-ems.service']);raise
for name,data in {payload!r}.items():
 p=pathlib.Path(name);p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(base64.b64decode(data))
os.chmod('/usr/local/sbin/gateway-disk-guard',0o755)
j=pathlib.Path('/etc/systemd/journald.conf.d/90-gateway-cap.conf');j.parent.mkdir(parents=True,exist_ok=True)
j.write_text('[Journal]\\nSystemMaxUse=256M\\nSystemKeepFree=7G\\nRuntimeMaxUse=64M\\n')
subprocess.run(['systemctl','restart','systemd-journald'],check=True)
subprocess.run(['journalctl','--rotate','--vacuum-size=256M'],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,check=True)
subprocess.run(['systemctl','daemon-reload'],check=True)
subprocess.run(['systemctl','enable','--now','gateway-disk-guard.timer'],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
print(json.dumps({{'changed':changed,'previousUi':str(old),'currentUi':str(dest),'disk':subprocess.check_output(['df','-h','/']).decode()}}))
""")
    (OUT/'release.json').write_text(json.dumps(release,indent=2),encoding='utf-8')
    print(json.dumps(release),flush=True)
    # No SQL mutation: archive the inactive legacy file byte-for-byte.
    meta=run(header+"""
p=root/'data/mqtt_event_outbox.db'
assert not owners(p),'Legacy database still open'
assert not pathlib.Path(str(p)+'-wal').exists() and not pathlib.Path(str(p)+'-journal').exists(),'Unmerged sidecar'
s=p.stat();print(json.dumps({'size':s.st_size,'mtime':s.st_mtime_ns,'inode':s.st_ino}))
""")
    assert shutil.disk_usage(OUT).free > meta['size']+1024**3
    archive=OUT/'mqtt_event_outbox.db.gz'
    _,o,e=c.exec_command('nice -n 19 gzip -1 -c /opt/modbus-gateway/data/mqtt_event_outbox.db',timeout=600)
    with archive.open('wb') as f:
        while True:
            chunk=o.read(1024*1024)
            if not chunk:break
            f.write(chunk)
        f.flush();os.fsync(f.fileno())
    if o.channel.recv_exit_status():raise RuntimeError(e.read().decode())
    h=hashlib.sha256();size=0
    with gzip.open(archive,'rb') as f:
        for chunk in iter(lambda:f.read(1024*1024),b''):h.update(chunk);size+=len(chunk)
    assert size==meta['size']
    result=run(header+f"""
p=root/'data/mqtt_event_outbox.db';s=p.stat()
assert (s.st_size,s.st_mtime_ns,s.st_ino)==({meta['size']},{meta['mtime']},{meta['inode']})
assert not owners(p)
h=hashlib.sha256()
with p.open('rb') as f:
 for chunk in iter(lambda:f.read(1048576),b''):h.update(chunk)
assert h.hexdigest()=={h.hexdigest()!r}
assert not owners(p) and p.stat().st_mtime_ns=={meta['mtime']}
assert not pathlib.Path(str(p)+'-wal').exists() and not pathlib.Path(str(p)+'-journal').exists()
p.unlink()
print(json.dumps({{'archivedSha':h.hexdigest(),'removedBytes':{size},'disk':subprocess.check_output(['df','-h','/']).decode()}}))
""",timeout=300)
    result['archive']=str(archive)
    (OUT/'archive-result.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
    print(json.dumps(result),flush=True)
finally:c.close()
