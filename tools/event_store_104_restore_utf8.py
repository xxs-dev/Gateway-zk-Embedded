#!/usr/bin/env python3
"""Restore old-parser UTF-8 compatibility without changing configuration values."""
import hashlib
import json
import os
from pathlib import Path
import subprocess

root=Path('/opt/modbus-gateway')
work=root/'releases/event-store-104-r05-20260908/cutover/plan'
assert json.loads((root/'config/runtime/device_identity.json').read_text())['machineCode']=='COMM202600104'
state=json.loads((work/'state.json').read_text());assert state['phase']=='rolled-back'
app=root/'config/runtime/apps/mqtt-service.json'
old=app.read_bytes();value=json.loads(old)
assert value['mqtt']['offlineBuffer']['eventOutbox']['sqlitePath']==state['rollback_output']+'/events.db'
assert 'eventStore' not in value
new=(json.dumps(value,ensure_ascii=False,indent=2)+'\n').encode('utf-8')
assert json.loads(new)==value
backup=work/'rollback-before-utf8.json'
with backup.open('xb') as f:f.write(old);f.flush();os.fsync(f.fileno())
tmp=app.with_name('mqtt-service.utf8-recovery.tmp')
with tmp.open('xb') as f:f.write(new);f.flush();os.fsync(f.fileno())
os.chmod(tmp,app.stat().st_mode&0o777)
os.replace(tmp,app)
fd=os.open(str(app.parent),os.O_DIRECTORY);os.fsync(fd);os.close(fd)
state.setdefault('rollback_configs',{}).setdefault(str(app),[]).append(hashlib.sha256(new).hexdigest())
temp=work/'state.utf8.tmp'
with temp.open('xb') as f:f.write((json.dumps(state,indent=2)+'\n').encode());f.flush();os.fsync(f.fileno())
os.replace(temp,work/'state.json')
units=['event-engine@mqtt-service.service','mqtt-driver@mqtt-service.service','mqtt-forwarder@mqtt-service.service']
subprocess.run(['systemctl','reset-failed',*units],check=True)
for unit in units:subprocess.run(['systemctl','--job-mode=ignore-dependencies','restart',unit],check=True)
print(json.dumps({'configurationValuesUnchanged':True,'encoding':'UTF-8 literal Unicode','sha256':hashlib.sha256(new).hexdigest()}))
