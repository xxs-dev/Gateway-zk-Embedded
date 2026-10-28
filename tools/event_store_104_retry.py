#!/usr/bin/env python3
"""Authorized r5 retry from the current 104 legacy rollback databases."""
import argparse
from pathlib import Path
import signal
import sys

import event_store_104_cutover as c


RELEASE=Path('/opt/modbus-gateway/releases/event-store-104-r5-20260909')
DATA=Path('/opt/modbus-gateway/data/event-store-104-r5-20260909')
CONFIG_GENERATION='canary-r5-20260909'
ROLLBACK=Path('/opt/modbus-gateway/data/event-store-104-r05/rollback-yx2t6usm')


def configure(route_evidence):
    expected=c.deploy.read_json(route_evidence)['appSha256']
    c.configs_unchanged({str(c.APP):expected})
    app=c.deploy.read_json(c.APP)
    store=app.get('eventStore',{})
    c.deploy.require(isinstance(store,dict) and store.get('backend','legacy')=='legacy',
                     'retry requires legacy backend, never IPC')
    archive=app['mqtt']['offlineBuffer']['eventOutbox']['sqlitePath']
    history=app['alarmStore']['sqlitePath']
    sources=[]
    for value,name in ((archive,'events.db'),(history,'history.db')):
        c.deploy.require(isinstance(value,str) and value==str(ROLLBACK/name),
                         'retry source must be the exact previous rollback path')
        path=c.deploy.safe_path(value)
        c.deploy.require(path.is_file(),'retry source must be an existing regular file')
        sources.append(path)
    c.configs_unchanged({str(c.APP):expected})
    c.RELEASE=RELEASE
    c.CONTROL=RELEASE/'cutover'
    c.DATA=DATA
    c.ARCHIVE,c.HISTORY=sources
    c.CONFIG_GENERATION=CONFIG_GENERATION
    c.EXTRACT_EXTRA_ARGS=['--allow-rollback-schema']
    c.HISTORY_EXTRA_ARGS=['--rebind-rollback-history']


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--execute',action='store_true',required=True)
    parser.add_argument('--route-evidence',required=True,type=Path)
    args=parser.parse_args()
    configure(args.route_evidence)
    return c.main()


if __name__=='__main__':
    def interrupted(signum,frame):
        raise RuntimeError('cutover interrupted by signal '+str(signum))
    signal.signal(signal.SIGTERM,interrupted)
    sys.exit(main())
