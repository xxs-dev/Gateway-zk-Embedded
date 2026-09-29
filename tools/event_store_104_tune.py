#!/usr/bin/env python3
"""Versioned 104 canary delivery cadence adjustment, preserving DB and rollback lineage."""
import copy
import json
import os
from pathlib import Path
import time
import event_store_canary_deploy as d

WORK=Path('/opt/modbus-gateway/releases/event-store-104-r05-20260908/cutover/plan')
UNITS=['mqtt-forwarder@mqtt-service.service','mqtt-driver@mqtt-service.service']


def main():
    os.umask(0o077)
    plan,state=d.load(WORK)
    d.require(state['phase']=='active','canary not active')
    d.require(d.read_json(plan['spec']['identity'])['machineCode']==d.IDENTITY,'wrong device')
    d.verify(plan,WORK)
    folder=WORK/'tune-delivery-200ms'
    d.require(not folder.exists(),'tuning already attempted; inspect evidence')
    d.require(len(plan['configs'])==1,'unexpected multiple configs')
    app=Path(next(iter(plan['configs'])))
    old=app.read_bytes()
    value=d.read_json(app)
    before=value['mqttDriver'].get('deliveryMaxLatencyMs',-1)
    d.require(before in (-1,0,1000),'unexpected latency setting')
    value['mqttDriver']['deliveryMaxLatencyMs']=200
    d.require(d.diff_paths(d.read_json(app),value)==[('mqttDriver','deliveryMaxLatencyMs')],'unexpected diff')
    folder.mkdir(mode=0o700)
    for name,path in [('app-before.json',app),('plan-before.json',WORK/'plan.json'),('state-before.json',WORK/'state.json')]:
        d.durable(folder/name,path.read_bytes())
    candidate=folder/'app-200ms.json'
    d.save(candidate,value)
    updated=copy.deepcopy(plan)
    updated['configs'][str(app)]['activated']=str(candidate)
    updated['hashes'][str(candidate)]=d.digest(candidate)
    updated['maintenance']=[*updated.get('maintenance',[]),dict(at=time.time(),
        parameter='mqttDriver.deliveryMaxLatencyMs',before=before,after=200,
        previousPlanSha256=d.digest(WORK/'plan.json'),evidence=str(folder))]
    d.save(folder/'plan-200ms.json',updated)
    sealed=dict(state,plan_sha256=d.digest(folder/'plan-200ms.json'))
    d.save(folder/'state-200ms.json',sealed)
    stopped=False
    try:
        stopped=True
        for unit in UNITS:d.systemctl('--job-mode=ignore-dependencies','stop',unit)
        d.stopped(UNITS)
        d.require(app.read_bytes()==old,'configuration changed while stopping')
        d.durable(app,candidate.read_bytes(),True)
        d.durable(WORK/'plan.json',(folder/'plan-200ms.json').read_bytes(),True)
        d.durable(WORK/'state.json',(folder/'state-200ms.json').read_bytes(),True)
        for unit in reversed(UNITS):d.systemctl('--job-mode=ignore-dependencies','start',unit)
        result=d.verify(*[updated,WORK])
        result.update(parameter='mqttDriver.deliveryMaxLatencyMs',before=before,after=200,
                      eventStoreAndEventEngineRestarted=False)
        d.save(folder/'result.json',result)
        print(json.dumps(result))
    except BaseException:
        if stopped:
            for unit in UNITS:d.systemctl('--job-mode=ignore-dependencies','stop',unit)
            d.stopped(UNITS)
            # Restore config/plan only; no event database is ever restored or replaced.
            d.require(app.read_bytes() in (old,candidate.read_bytes()),'foreign config change; manual recovery')
            for target,source in [(app,'app-before.json'),(WORK/'plan.json','plan-before.json'),(WORK/'state.json','state-before.json')]:
                d.durable(target,(folder/source).read_bytes(),True)
            for unit in reversed(UNITS):d.systemctl('--job-mode=ignore-dependencies','start',unit)
        raise


if __name__=='__main__':main()
