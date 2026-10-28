#!/usr/bin/env python3
"""Read-only resource and EventStore observations on the authorized canary."""
import argparse
import contextlib
import datetime
import json
import os
from pathlib import Path
import socket
import sqlite3
import struct
import subprocess
import time


def rpc(config, operation, args=None):
    body = json.dumps(dict(version='1', storeId=config['storeId'],
                           configGeneration=config['configGeneration'],
                           op=operation, args=args or {})).encode('utf-8')
    with socket.socket(socket.AF_UNIX) as peer:
        peer.settimeout(5)
        peer.connect(config['socketPath'])
        peer.sendall(struct.pack('!I', len(body)) + body)
        def read_exact(size):
            result = b''
            while len(result) < size:
                chunk = peer.recv(size - len(result))
                if not chunk:
                    raise RuntimeError('short IPC reply')
                result += chunk
            return result
        size = struct.unpack('!I', read_exact(4))[0]
        if size > 262144:
            raise RuntimeError('oversized IPC reply')
        return json.loads(read_exact(size))


def sample(config=None):
    now = time.monotonic()
    result = dict(atUtc=datetime.datetime.utcnow().isoformat()+'Z', monotonic=now,
                  clockTicks=os.sysconf('SC_CLK_TCK'), processes=[], databaseFds=[])
    for p in Path('/proc').iterdir():
        if not p.name.isdigit():
            continue
        try:
            exe = os.readlink(p/'exe')
            if Path(exe).name not in ('EventStore','EventEngine','MqttDriver','MqttForwarder',
                                      'ComputeEngine','KY-EMS','SystemMonitor'):
                continue
            stat = (p/'stat').read_text().rsplit(')', 1)[1].split()
            status = (p/'status').read_text().splitlines()
            row = dict(pid=int(p.name), exe=exe, startTicks=int(stat[19]),
                       cpuTicks=int(stat[11])+int(stat[12]),
                       rssKiB=next(int(s.split()[1]) for s in status if s.startswith('VmRSS:')))
            try:
                row['io'] = {k.strip():int(v) for k,v in
                             (line.split(':',1) for line in (p/'io').read_text().splitlines())}
            except OSError as error:
                row['io'] = None
                row['ioUnavailable'] = str(error)
            result['processes'].append(row)
            for fd in (p/'fd').iterdir():
                try:
                    target = os.readlink(fd)
                    if not any(s in target for s in ('events.db','history.db','mqtt_event_outbox.db','alarm_events.db')):
                        continue
                    info = dict(line.split(':',1) for line in (p/'fdinfo'/fd.name).read_text().splitlines())
                    result['databaseFds'].append(dict(pid=int(p.name),exe=exe,path=target,
                                                     writable=(int(info['flags'].strip(),8)&3)!=0))
                except (OSError, KeyError, ValueError):
                    pass
        except (OSError, StopIteration, ValueError):
            continue
    result['memoryKiB'] = {line.split(':',1)[0]:int(line.split()[1])
                           for line in Path('/proc/meminfo').read_text().splitlines()
                           if line.split(':',1)[0] in ('MemTotal','MemAvailable','SwapFree')}
    result['load'] = Path('/proc/loadavg').read_text().strip()
    disk = os.statvfs('/opt/modbus-gateway')
    result['availableBytes'] = disk.f_bavail*disk.f_frsize
    if config:
        result['ipc'] = {}
        for op in ('Hello','GetCapacityStatus','GetHistoryProjectionStatus'):
            try:
                result['ipc'][op] = rpc(config,op)
            except (OSError,ValueError,RuntimeError) as e:
                result['ipc'][op] = {'error':str(e)}
        result['dbBytes'] = {suffix:Path(config['databasePath']+suffix).stat().st_size
                             for suffix in ('','-wal','-shm') if Path(config['databasePath']+suffix).exists()}
        try:
            with contextlib.closing(sqlite3.connect('file:'+config['databasePath']+'?mode=ro',uri=True,timeout=1)) as db:
                result['pendingStats'] = db.execute('SELECT * FROM mqtt_event_outbox_stats').fetchall()
                result['sourceHighId'] = db.execute('SELECT id FROM mqtt_event_outbox ORDER BY id DESC LIMIT 1').fetchall()
                result['journalHighId'] = db.execute('SELECT id FROM event_local_journal ORDER BY id DESC LIMIT 1').fetchall()
        except sqlite3.Error as error:
            result['databaseReadError'] = str(error)
    return result


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--store-config',type=Path)
    p.add_argument('--samples',type=int,default=7)
    p.add_argument('--interval',type=float,default=10)
    a=p.parse_args()
    if not 1 <= a.samples <= 361 or not 0 < a.interval <= 60:
        p.error('samples must be 1..361 and interval 0..60 seconds')
    config=json.loads(a.store_config.read_text()) if a.store_config else None
    previous=None
    for n in range(a.samples):
        current=sample(config)
        if previous:
            old={(x['pid'],x['startTicks']):x for x in previous['processes']}
            elapsed=current['monotonic']-previous['monotonic']
            for x in current['processes']:
                before=old.get((x['pid'],x['startTicks']))
                if before:
                    x['cpuPercentOneCore']=round(100*(x['cpuTicks']-before['cpuTicks'])/current['clockTicks']/elapsed,2)
                    if x['io'] is not None and before['io'] is not None:
                        x['writeBytesPerSecond']=round((x['io']['write_bytes']-before['io']['write_bytes'])/elapsed,2)
        print(json.dumps(current),flush=True)
        previous=current
        if n+1<a.samples:
            time.sleep(a.interval)


if __name__=='__main__':
    main()
