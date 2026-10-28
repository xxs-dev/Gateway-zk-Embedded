#!/usr/bin/env python3
"""End the 104 canary using incremental recovery, retaining every database."""
import json
import os
import time
import event_store_104_cutover as c


def main():
    os.umask(0o077)
    c.deploy.require(c.deploy.read_json(c.IDENTITY)['machineCode']==c.deploy.IDENTITY,'wrong identity')
    plan,state=c.deploy.load(c.CONTROL/'plan')
    c.deploy.require(state['phase']=='active','canary not active')
    protected=['compute-engine@mqtt-service.service','system-monitor@monitor-service.service','ky-ems.service']
    before={u:c.service('show',u,'--property=MainPID','--property=ExecMainStartTimestampMonotonic') for u in protected}
    c.emit('rollback-started',reason='sustained MQTT backlog during real-network canary')
    try:
        c.recover(c.CONTROL/'original.json',c.deploy.read_json(c.CONTROL/'permissions.json'),True)
        after={u:c.service('show',u,'--property=MainPID','--property=ExecMainStartTimestampMonotonic') for u in protected}
        c.deploy.require(before==after,'protected process identity changed')
        plan,state=c.deploy.load(c.CONTROL/'plan')
        c.deploy.require(state['phase']=='rolled-back','incremental rollback not committed')
        c.emit('rolled-back',atCompleted=time.time(),data=state['rollback_output'],
               protectedProcessesUnchanged=True,databasesPreserved=True)
    except BaseException as error:
        c.emit('recovery-failed',errorType=type(error).__name__)
        raise


if __name__=='__main__':main()
