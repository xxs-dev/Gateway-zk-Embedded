#!/usr/bin/env python3
"""One-shot authorized 104 canary; run under a local systemd transient unit."""
import argparse
import contextlib
import hashlib
import json
import os
from pathlib import Path
import signal
import sqlite3
import subprocess
import sys
import time

import event_store_canary_deploy as deploy

ROOT=Path('/opt/modbus-gateway')
RELEASE=ROOT/'releases/event-store-104-r05-20260908'
CONTROL=RELEASE/'cutover'
DATA=ROOT/'data/event-store-104-r05'
CONFIG_GENERATION='canary-r05-20260908'
EXTRACT_EXTRA_ARGS=[]
HISTORY_EXTRA_ARGS=[]
APP=ROOT/'config/runtime/apps/mqtt-service.json'
IDENTITY=ROOT/'config/runtime/device_identity.json'
ARCHIVE=ROOT/'data/mqtt_event_outbox.db'
HISTORY=ROOT/'data/alarm_events.db'
PAIRS=[('event-engine@mqtt-service.service','EventEngine'),
       ('mqtt-driver@mqtt-service.service','MqttDriver'),
       ('mqtt-forwarder@mqtt-service.service','MqttForwarder')]
UNITS=[x[0] for x in PAIRS]
STORE_FILE=Path('/etc/systemd/system')/deploy.STORE_UNIT
START_GATE=Path('/run/event-store-cutover-104.ready')
GUARD_TEXT='[Unit]\nConditionPathExists='+str(START_GATE)+'\n'
GUARDS=[Path('/run/systemd/system')/(u+'.d')/'99-event-store-cutover-104.conf' for u in UNITS]
MARKERS=['/run/gateway-health-watchdog/applying',
         str(ROOT/'ota/staging/ota_status_pending.log')]


def save(name, value):
    deploy.save(CONTROL/name,value,True)


def emit(phase, **values):
    report=dict(phase=phase,at=time.time(),**values)
    save('status.json',report)
    print(json.dumps(report),flush=True)


def run(*args, timeout=1200):
    return subprocess.run([str(x) for x in args],check=True,stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE,universal_newlines=True,timeout=timeout).stdout


def invoke(tool, args, report):
    value=json.loads(run(sys.executable,RELEASE/tool,*args))
    save(report,value)
    return value


def configs_unchanged(expected):
    for path,digest in expected.items():
        deploy.require(deploy.digest(path)==digest,'configuration changed: '+path)


def gate():
    deploy.require(not any(Path(p).exists() for p in MARKERS),'OTA marker appeared')
    save('gate.json',dict(machineCode=deploy.IDENTITY,otaIdle=True,restartInhibited=True,
                          expires=time.time()+290))
    return CONTROL/'gate.json'


def service(*args):
    return run('systemctl',*args,timeout=180)


def stop(units):
    # Explicit stops suppress systemd's normal Restart= behavior.
    for unit in units:
        service('--job-mode=ignore-dependencies','stop',unit)
    deploy.stopped(units)


def remove_guards():
    for path in GUARDS:
        if path.exists():
            deploy.require(path.read_text()==GUARD_TEXT,'runtime guard changed')
            path.unlink()
    if START_GATE.exists():
        START_GATE.unlink()
    service('daemon-reload')


def recover(original, permissions, unmasked):
    if START_GATE.exists():
        START_GATE.unlink()
    stop(UNITS)
    if STORE_FILE.exists():
        stop([deploy.STORE_UNIT])
    if deploy.digest(APP)==deploy.digest(original) and not STORE_FILE.exists():
        for path,mode in permissions.items():
            Path(path).chmod(mode)
    else:
        invoke('event_store_canary_deploy.py',['rollback','--work',str(CONTROL/'plan'),
                                              '--gate',str(gate())],'rollback.json')
    # Never start either writer against a partially recovered database.
    if not unmasked:
        service('unmask','--runtime',*UNITS)
    remove_guards()
    for unit in UNITS:
        service('--job-mode=ignore-dependencies','start',unit)
        deploy.require(service('show',unit,'--property=ActiveState','--value').strip()=='active',
                       'restored service failed: '+unit)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--execute',action='store_true',required=True)
    p.add_argument('--route-evidence',required=True,type=Path)
    a=p.parse_args()
    deploy.require(os.geteuid()==0 and sys.platform=='linux','Linux root required')
    deploy.require(json.loads(IDENTITY.read_text())['machineCode']==deploy.IDENTITY,'wrong device')
    evidence=json.loads(a.route_evidence.read_text())
    expected={str(APP):evidence['appSha256'],**evidence['deviceFileHashes']}
    configs_unchanged(expected)
    deploy.require(not CONTROL.exists() and not DATA.exists(),'cutover paths already exist')
    deploy.require(not any(Path(x).exists() for x in MARKERS),'OTA not idle')
    deploy.require(not STORE_FILE.exists(),'store unit exists')
    deploy.require(not START_GATE.exists() and not any(x.exists() for x in GUARDS),'start guard exists')
    for unit in UNITS:
        deploy.require(service('show',unit,'--property=ActiveState','--value').strip()=='active',
                       'original service not active: '+unit)
        deploy.require(not (Path('/run/systemd/system')/unit).exists(),'runtime unit override exists')
    os.umask(0o077)
    CONTROL.mkdir();DATA.mkdir()
    (CONTROL/'backups').mkdir()
    original=CONTROL/'original.json'
    original.write_bytes(APP.read_bytes())
    permissions={str(path):path.stat().st_mode&0o777 for path in (ARCHIVE,HISTORY)}
    save('permissions.json',permissions)
    protected=['compute-engine@mqtt-service.service','system-monitor@monitor-service.service','ky-ems.service']
    before={u:service('show',u,'--property=MainPID','--property=ExecMainStartTimestampMonotonic') for u in protected}
    save('protected-before.json',before)
    stopped_any=False
    unmasked=False
    try:
        emit('stopping-event-services')
        stopped_any=True
        for path in GUARDS:
            path.parent.mkdir(parents=True,exist_ok=True)
            deploy.durable(path,GUARD_TEXT.encode())
        service('daemon-reload')
        service('mask','--runtime',*UNITS)
        stop(UNITS)
        deploy.no_accessors([ARCHIVE,HISTORY])
        configs_unchanged(expected)
        deploy.require(not any(Path(x).exists() for x in MARKERS),'OTA appeared before freeze')
        for path in (ARCHIVE,HISTORY):
            path.chmod(0o444)
            for suffix in ('-wal','-journal'):
                side=Path(str(path)+suffix)
                if side.exists():
                    permissions[str(side)]=side.stat().st_mode&0o777
                    side.chmod(0o444)
        save('permissions.json',permissions)
        emit('extracting-frozen-workset')
        source=CONTROL/'workset.db'
        invoke('event_store_extract_workset.py',[
            'extract','--source',str(ARCHIVE),'--target',str(source),
            '--machine-code',deploy.IDENTITY,'--offline','--source-frozen',
            '--page-size','4096','--page-timeout-ms','500',
            '--max-workset-bytes',str(64*1024*1024),'--reserve-free-bytes',str(2*1024**3)]+EXTRACT_EXTRA_ARGS,'extract.json')
        with contextlib.closing(sqlite3.connect('file:'+str(source)+'?mode=ro',uri=True)) as db:
            owners={row[0]:'event-engine' for row in db.execute('SELECT state_key FROM mqtt_event_state')}
        save('owners.json',owners)
        emit('migrating-workset')
        migration=invoke('event_store_migrate.py',[
            'migrate','--source',str(source),'--target',str(DATA/'events.db'),
            '--backup-dir',str(CONTROL/'backups'),'--offline','--store-id','comm104-events',
            '--config-generation',CONFIG_GENERATION,'--state-owners',str(CONTROL/'owners.json')], 'migrate.json')
        baseline=Path(migration['cutover_baseline']);baseline.chmod(0o444)
        invoke('event_store_migrate.py',[
            'history','--source',str(HISTORY),'--target',str(DATA/'history.db'),
            '--backup-dir',str(CONTROL/'backups'),'--offline','--event-store',str(DATA/'events.db'),
            '--history-id','comm104-legacy-history']+HISTORY_EXTRA_ARGS, 'history.json')
        lab=json.loads((RELEASE/'lab-template.json').read_text())
        overlay=json.loads((RELEASE/'overlay-template.json').read_text())
        for obj in (lab,overlay['eventStore']):
            obj.update(storeId='comm104-events',configGeneration=CONFIG_GENERATION,storageProfile='wal-full')
        lab.update(maxStoreBytes=1024**3,minFreeBytes=2*1024**3)
        save('lab-template.json',lab);save('overlay-template.json',overlay)
        spec=dict(release=str(RELEASE),data=str(DATA),identity=str(IDENTITY),baseline=str(baseline),
                  source=str(source),archive_source=str(ARCHIVE),extract_manifest=str(source)+'.manifest.json',
                  history_source=str(HISTORY),migration_report=str(CONTROL/'migrate.json'),
                  history_report=str(CONTROL/'history.json'),history_id='comm104-legacy-history',
                  lab_template=str(CONTROL/'lab-template.json'),overlay_template=str(CONTROL/'overlay-template.json'),
                  services=[dict(unit=u,binary=b,config=str(APP),original=str(original)) for u,b in PAIRS],
                  ota_markers=MARKERS,reserve_bytes=2*1024**3,start_gate=str(START_GATE))
        save('spec.json',spec)
        service('unmask','--runtime',*UNITS);unmasked=True
        invoke('event_store_canary_deploy.py',['prepare','--spec',str(CONTROL/'spec.json'),
                                              '--work',str(CONTROL/'plan')],'prepare.json')
        configs_unchanged(expected)
        emit('activating-canary')
        invoke('event_store_canary_deploy.py',['activate','--work',str(CONTROL/'plan'),
                                              '--gate',str(gate())],'activate.json')
        after={u:service('show',u,'--property=MainPID','--property=ExecMainStartTimestampMonotonic') for u in protected}
        deploy.require(before==after,'protected process identity changed')
        configs_unchanged(evidence['deviceFileHashes'])
        remove_guards()
        emit('active',data=str(DATA),work=str(CONTROL/'plan'),protectedProcessesUnchanged=True)
        return 0
    except BaseException as error:
        emit('recovering',errorType=type(error).__name__)
        try:
            if stopped_any:
                recover(original,permissions,unmasked)
        except BaseException as recovery:
            emit('recovery-failed',errorType=type(error).__name__,recoveryErrorType=type(recovery).__name__,
                 action='preserve all databases and repair recovery before starting event services')
            return 2
        emit('failed-restored',errorType=type(error).__name__)
        return 1


if __name__=='__main__':
    def interrupted(signum, frame):
        raise RuntimeError('cutover interrupted by signal '+str(signum))
    signal.signal(signal.SIGTERM,interrupted)
    sys.exit(main())
