#!/usr/bin/env python3
"""Read-only post-canary reconciliation and backlog drain check."""
import contextlib
import json
from pathlib import Path
import sqlite3
import subprocess
import time
import argparse

parser=argparse.ArgumentParser()
parser.add_argument('--samples',type=int,default=13)
args=parser.parse_args()
assert 1<=args.samples<=121

root=Path('/opt/modbus-gateway')
work=root/'releases/event-store-104-r05-20260908/cutover/plan'
plan=json.loads((work/'plan.json').read_text());state=json.loads((work/'state.json').read_text())
assert state['phase']=='rolled-back'
config=json.loads((root/'config/runtime/apps/mqtt-service.json').read_text())
path=config['mqtt']['offlineBuffer']['eventOutbox']['sqlitePath']
assert path==state['rollback_output']+'/events.db'
assert 'eventStore' not in config
def events(filename):
    rows={};cursor=0
    with contextlib.closing(sqlite3.connect('file:'+str(filename)+'?mode=ro',uri=True,timeout=1)) as db:
        maximum=db.execute('SELECT id FROM mqtt_event_outbox ORDER BY id DESC LIMIT 1').fetchone()[0]
        while cursor<maximum:
            deadline=time.monotonic()+2
            db.set_progress_handler(lambda:int(time.monotonic()>deadline),1000)
            page=db.execute('SELECT id,event_id,target_id,event_type,topic,payload,event_ts,sent FROM mqtt_event_outbox WHERE id>? AND id<=? ORDER BY id LIMIT 256',(cursor,maximum)).fetchall()
            if not page:break
            for row in page:rows[(row[1],row[2])]=row
            cursor=page[-1][0]
    return rows
trial=events(plan['spec']['data']+'/events.db')
for n in range(args.samples):
    result={'at':time.time(),'database':path}
    with contextlib.closing(sqlite3.connect('file:'+path+'?mode=ro',uri=True,timeout=3)) as db:
        deadline=time.monotonic()+3
        db.set_progress_handler(lambda:int(time.monotonic()>deadline),1000)
        result['pending']=db.execute('SELECT count(*),min(created_at),max(retry_count) FROM mqtt_event_outbox WHERE sent=0').fetchone()
        result['highId']=db.execute('SELECT id FROM mqtt_event_outbox ORDER BY id DESC LIMIT 1').fetchone()[0]
        if n in (0,args.samples-1):
            result['quickCheck']=db.execute('PRAGMA quick_check').fetchall()
            result['states']=db.execute('SELECT count(*) FROM mqtt_event_state').fetchone()[0]
    if n in (0,args.samples-1):
        current=events(path)
        result['trialEvents']=len(trial)
        result['missingTrialEvents']=len(set(trial)-set(current))
        result['changedTrialPayloads']=sum(t[3:7]!=current[k][3:7] for k,t in trial.items() if k in current)
        result['lostAcknowledgements']=sum(t[7]==1 and current[k][7]!=1 for k,t in trial.items() if k in current)
        result['units']={}
        for u in ['event-engine@mqtt-service.service','mqtt-driver@mqtt-service.service','mqtt-forwarder@mqtt-service.service','compute-engine@mqtt-service.service','system-monitor@monitor-service.service','ky-ems.service','event-store-canary-104.service']:
            raw=subprocess.check_output(['systemctl','show',u,'--property=ActiveState','--property=MainPID','--property=NRestarts','--property=ExecMainStartTimestampMonotonic'],universal_newlines=True)
            result['units'][u]=dict(x.split('=',1) for x in raw.splitlines() if '=' in x)
    print(json.dumps(result),flush=True)
    if n+1<args.samples:time.sleep(10)
