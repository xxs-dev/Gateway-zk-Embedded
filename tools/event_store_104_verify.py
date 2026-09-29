#!/usr/bin/env python3
"""Read-only live reconciliation for the 104 canary, no control commands."""
import contextlib
import json
from pathlib import Path
import sqlite3
import subprocess
import time
import event_store_canary_deploy as deploy

root=Path('/opt/modbus-gateway')
work=root/'releases/event-store-104-r05-20260908/cutover/plan'
plan,state=deploy.load(work)
result={'at':time.time(),'verify':deploy.verify(plan,work)}
config=json.loads((work/'lab.json').read_text())
with contextlib.closing(sqlite3.connect('file:'+config['databasePath']+'?mode=ro',uri=True,timeout=3)) as db:
    db.execute('BEGIN')
    result['quickCheck']=db.execute('PRAGMA quick_check').fetchall()
    result['counts']=db.execute('SELECT event_type,sent,count(*) FROM mqtt_event_outbox GROUP BY event_type,sent').fetchall()
    result['pendingAges']=db.execute('SELECT count(*),min(created_at),max(created_at),max(retry_count) FROM mqtt_event_outbox WHERE sent=0').fetchall()
    result['recentAcks']=db.execute('SELECT event_id,sent_at,retry_count,last_error FROM mqtt_event_outbox WHERE sent=1 ORDER BY id DESC LIMIT 10').fetchall()
    result['journalCount']=db.execute('SELECT count(*) FROM event_local_journal').fetchone()[0]
    result['projectionCursor']=db.execute('SELECT * FROM event_history_projection_cursor').fetchall()
    result['states']=db.execute('SELECT count(*) FROM mqtt_event_state').fetchone()[0]
    db.execute('COMMIT')
with contextlib.closing(sqlite3.connect('file:'+config['historyPath']+'?mode=ro',uri=True,timeout=3)) as db:
    result['historyQuickCheck']=db.execute('PRAGMA quick_check').fetchall()
    result['historyCount']=db.execute('SELECT count(*) FROM alarm_events').fetchone()[0]
result['units']={}
for unit in [deploy.STORE_UNIT]+[s['unit'] for s in plan['spec']['services']]:
    raw=subprocess.check_output(['systemctl','show',unit,'--property=NRestarts','--property=MainPID',
                                 '--property=ActiveState','--property=ExecMainStatus'],universal_newlines=True)
    result['units'][unit]=dict(line.split('=',1) for line in raw.splitlines() if '=' in line)
    raw=subprocess.check_output(['journalctl','-u',unit,'_PID='+result['units'][unit]['MainPID'],
                                 '-n','120','--no-pager','-o','cat'],universal_newlines=True)
    lines=[line for line in raw.splitlines() if any(x in line.lower() for x in ('error','failed','connect','puback','publish','ready','delegat','event store','ipc'))]
    result.setdefault('logSummary',{})[unit]=lines[-40:]
print(json.dumps(result))
