#!/usr/bin/env python3
"""Read-only state/route eligibility evidence; does not replace the C++ loader."""
import contextlib
import hashlib
import json
from pathlib import Path
import sqlite3


def main():
    root=Path('/opt/modbus-gateway')
    app=root/'config/runtime/apps/mqtt-service.json'
    config=json.loads(app.read_text())
    assert json.loads(Path(config['identityConfigFile']).read_text())['machineCode']=='COMM202600104'
    routes={}
    files={}
    duplicates=[]
    def points(node, source):
        if isinstance(node,dict):
            for p in node.get('points',[]):
                if 'index' in p:
                    index=int(p['index'])
                    if index in routes:
                        duplicates.append(index)
                    routes[index]=dict(index=index,reportOnChange=p.get('reportOnChange',False),
                                       alarms=p.get('alarms',[]),source=source)
            for key,value in node.items():
                if key!='points':
                    points(value,source)
        elif isinstance(node,list):
            for item in node:
                points(item,source)
    for filename in config['deviceConfigFiles']:
        path=Path(filename)
        data=path.read_bytes()
        files[filename]=hashlib.sha256(data).hexdigest()
        points(json.loads(data),filename)
    source=config['mqtt']['offlineBuffer']['eventOutbox']['sqlitePath']
    with contextlib.closing(sqlite3.connect('file:'+source+'?mode=ro',uri=True,timeout=3)) as db:
        rows=db.execute('SELECT state_key,event_type,point_index,alarm_type,lifecycle FROM mqtt_event_state').fetchall()
        examples=db.execute('SELECT id,event_id,event_type,sent FROM mqtt_event_outbox ORDER BY id LIMIT 3').fetchall()
    skipped=[]
    for key,kind,index,alarm,lifecycle in rows:
        route=routes.get(index)
        reason=None
        if route is None:
            reason='point absent from configured JSON routes'
        elif kind=='change' and not route['reportOnChange']:
            reason='reportOnChange disabled'
        elif kind=='alarm' and not any(str(a.get('type','')).lower()==alarm.lower() for a in route['alarms']):
            reason='alarm rule absent'
        if reason:
            skipped.append(dict(stateKey=key,index=index,lifecycle=lifecycle,reason=reason))
    print(json.dumps(dict(appSha256=hashlib.sha256(app.read_bytes()).hexdigest(),
                         deviceFileHashes=files,pointCount=len(routes),stateCount=len(rows),
                         duplicateIndexes=duplicates,statesNotRestorable=skipped,
                         oldestEvents=examples),ensure_ascii=True))


if __name__=='__main__':
    main()
