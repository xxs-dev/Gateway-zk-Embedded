"""Read-only baseline probe for the two explicitly authorized lab hosts."""
import argparse
import base64
import json
import os
from pathlib import Path
import shlex
import paramiko

SCRIPT = r'''
import json, os, pathlib, platform, subprocess, datetime
p=pathlib.Path('/proc')
memory={}
for line in (p/'meminfo').read_text().splitlines():
    key, value=line.split(':',1)
    if key in ('MemTotal','MemAvailable','SwapTotal','SwapFree'): memory[key]=int(value.split()[0])
processes=[]
for entry in p.iterdir():
    if not entry.name.isdigit(): continue
    try:
        exe=os.readlink(str(entry/'exe'))
        if pathlib.Path(exe).name not in ('EventEngine','MqttDriver','MqttForwarder','EventStore'): continue
        status=(entry/'status').read_text().splitlines()
        rss=next((int(line.split()[1]) for line in status if line.startswith('VmRSS:')),None)
        processes.append(dict(pid=int(entry.name),exe=exe,rssKiB=rss,
            startTicks=(entry/'stat').read_text().rsplit(')',1)[1].split()[19]))
    except (OSError,ValueError,StopIteration): pass
disk=os.statvfs('/')
print(json.dumps(dict(observedAtUtc=datetime.datetime.utcnow().isoformat()+'Z',machine=platform.machine(),memoryKiB=memory,
    uptime=(p/'uptime').read_text().strip(),load=(p/'loadavg').read_text().strip(),
    rootFreeBytes=disk.f_bavail*disk.f_frsize,processes=processes,
    sqlitePythonVersion=__import__('sqlite3').sqlite_version)))
'''

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('host',choices=['192.168.22.11','192.168.22.16'])
    parser.add_argument('--output',required=True,type=Path)
    args=parser.parse_args()
    secret=os.environ.get('GATEWAY_LAB_PASSWORD')
    if not secret: raise RuntimeError('GATEWAY_LAB_PASSWORD required')
    client=paramiko.SSHClient()
    client.load_system_host_keys()
    client.set_missing_host_key_policy(paramiko.RejectPolicy())
    client.connect(args.host,username='root',password=secret,timeout=12,
                   auth_timeout=12,banner_timeout=12,look_for_keys=False,allow_agent=False)
    try:
        encoded=base64.b64encode(SCRIPT.encode()).decode()
        command='python3 -c '+shlex.quote('import base64;exec(base64.b64decode('+repr(encoded)+'))')
        _,out,err=client.exec_command(command,timeout=20)
        raw=out.read().decode(); error=err.read().decode()
        if out.channel.recv_exit_status()!=0: raise RuntimeError(error)
        result=json.loads(raw)
        args.output.parent.mkdir(parents=True,exist_ok=True)
        with args.output.open('x',encoding='utf-8') as output:
            json.dump(result,output,indent=2,ensure_ascii=False)
        print(json.dumps(result,ensure_ascii=False))
    finally: client.close()

if __name__=='__main__': main()
