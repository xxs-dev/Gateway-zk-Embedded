#!/usr/bin/env python3
"""Read-only status of 104 migration jobs; omit environment and arbitrary argv."""
import json
import os
from pathlib import Path
import subprocess
import time


result={'at':time.time(),'jobs':[]}
uptime=float(Path('/proc/uptime').read_text().split()[0])
ticks=os.sysconf('SC_CLK_TCK')
names=('event_store_extract_workset.py','event_store_104_cutover.py')
for p in Path('/proc').iterdir():
    if not p.name.isdigit() or int(p.name)==os.getpid():continue
    try:
        argv=(p/'cmdline').read_bytes().split(b'\0')
        matched=next((name for name in names if any(x.endswith(b'/'+name.encode()) for x in argv)),None)
        if not matched:continue
        stat=(p/'stat').read_text().rsplit(')',1)[1].split()
        result['jobs'].append(dict(pid=int(p.name),tool=matched,state=stat[0],
                                  elapsedSeconds=round(uptime-int(stat[19])/ticks,2),
                                  cpuSeconds=round((int(stat[11])+int(stat[12]))/ticks,2),
                                  rssBytes=int(stat[21])*os.sysconf('SC_PAGE_SIZE'),
                                  waitChannel=(p/'wchan').read_text().strip()))
    except (OSError,ValueError):pass
control=Path('/opt/modbus-gateway/releases/event-store-104-r05-20260908/cutover')
if (control/'status.json').exists():result['cutover']=json.loads((control/'status.json').read_text())
units=['event-store-cutover-104.service','event-store-canary-104.service',
       'event-engine@mqtt-service.service','mqtt-driver@mqtt-service.service','mqtt-forwarder@mqtt-service.service']
result['services']={}
for unit in units:
    p=subprocess.run(['systemctl','show',unit,'--property=ActiveState','--property=SubState',
                      '--property=MainPID','--property=NRestarts','--property=ExecMainStatus'],
                     stdout=subprocess.PIPE,stderr=subprocess.PIPE,universal_newlines=True)
    result['services'][unit]=dict(line.split('=',1) for line in p.stdout.splitlines() if '=' in line)
print(json.dumps(result))
