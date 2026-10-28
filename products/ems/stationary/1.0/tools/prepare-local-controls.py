"""Prepare (never deploy) the audited COMM202600103 local-control migration."""
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import secrets
import shutil

MODE, POWER, MANUAL, DIRECTION, PERMIT, PHASE = range(730100, 730106)
NONZERO = 730106
ZERO = 730107
FORCE_FULL = 730108
APPLIED = 730109
REMOTE, OVERRIDE, OVERRIDE_P, OVERRIDE_Q = range(730110, 730114)


def read(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def write(path, data):
    path.write_text(json.dumps(data, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')


def migrate_graph(source):
    graph = copy.deepcopy(source)
    nodes = {n['id']: n for n in graph['nodes']}
    if any(n.startswith('local_control_') for n in nodes):
        raise ValueError('Local controls already installed')
    required = ['power_arbiter', 'cycle_basic_safety_gate', 'cycle_phase_sequence',
                'cycle_charge_direction_allowed', 'cycle_discharge_direction_allowed']
    if any(n not in nodes for n in required):
        raise ValueError('Unsupported control graph')
    for i in range(6):
        p = nodes[f'cycle_pcs_control_write_{i}']['parameters']
        if p['targetIndex'] != 1318 + i or p['minValue'] != -41.6 or p['maxValue'] != 41.6:
            raise ValueError('Unexpected PCS write target or rating')
    def update(node_id, mutate):
        node = nodes[node_id]
        mutate(node['parameters'])
        # Keep the V2 editor ports consistent with runtime parameter changes.
        for port in node['ports']:
            value = node['parameters']
            for part in port['runtimePath'].strip('/').split('/'):
                value = value[int(part)] if isinstance(value, list) else value[part]
            if port.get('binding', {}).get('kind') == 'point':
                port['binding']['index'] = value

    expected_override={'enableIndex':400026,'enableValue':1,'activeTotalIndex':400590,'reactiveTotalIndex':400591}
    if nodes['power_arbiter']['parameters']['override'] != expected_override:
        raise ValueError('Unrecognized original override; manual review required')
    transitions=nodes['cycle_phase_sequence']['parameters']['transitions']
    expected_limits=[(1,2,'lte',20),(2,1,'gte',95)]
    for before,after,operator,limit in expected_limits:
        matches=[t for t in transitions if t['from']==before and t['to']==after]
        if len(matches)!=1 or matches[0]['conditions'] != [{'index':1569,'operator':operator,'value':limit}]:
            raise ValueError('Cycle SOC boundaries changed; review the local controls')
    update('power_arbiter', lambda p: p['override'].update(
        enableIndex=OVERRIDE, enableValue=1, activeTotalIndex=OVERRIDE_P, reactiveTotalIndex=OVERRIDE_Q))
    for node_id in ['force_full_soc_upper'] + [f'force_full_{kind}_{i}' for kind in ('active','reactive') for i in range(3)]:
        if nodes[node_id]['parameters']['conditionIndex'] != 400030:
            raise ValueError('Unexpected force-full control branch')
        update(node_id, lambda p: p.update(conditionIndex=FORCE_FULL))
    for suffix, direction, operator, limit in [('charge', 2, 'lt', 95), ('discharge', 1, 'gt', 20)]:
        def change(p):
            matched = [c for c in p['conditions'] if c.get('index') == 400017]
            if len(matched) != 1 or matched[0]['value'] != direction:
                raise ValueError('Unexpected direction safety gate')
            matched[0]['index'] = PHASE
            p['conditions'].append({'index':1569, 'operator':operator, 'value':limit})
        update(f'cycle_{suffix}_direction_allowed', change)
    nodes['cycle_basic_safety_gate']['parameters']['conditions'].append(
        {'index':PERMIT, 'operator':'eq', 'value':1})
    nodes['cycle_basic_safety_gate']['parameters']['conditions'].append(
        {'index':3999, 'operator':'eq', 'value':1})
    definitions = [
        ('remote', 'controlGate', {'outputIndex':REMOTE,'combine':'all',
         'conditions':[{'index':MODE,'operator':'eq','value':1},{'index':400026,'operator':'eq','value':1}]}),
        ('override', 'controlGate', {'outputIndex':OVERRIDE,'combine':'any',
         'conditions':[{'index':MANUAL,'operator':'eq','value':1},{'index':REMOTE,'operator':'eq','value':1}]}),
        ('override_p','switch',{'outputIndex':OVERRIDE_P,'conditionIndex':MANUAL,'trueIndex':POWER,'falseIndex':400590}),
        ('override_q','switch',{'outputIndex':OVERRIDE_Q,'conditionIndex':MANUAL,'trueValue':0,'falseIndex':400591}),
        ('applied', 'formula', {'outputIndex':APPLIED,'operation':'add',
         'inputs':[{'index':MODE},{'value':0}]}),
        ('force_full', 'controlGate', {'outputIndex':FORCE_FULL,'combine':'all',
         'conditions':[{'index':MODE,'operator':'eq','value':1},{'index':400030,'operator':'eq','value':1}]}),
        ('zero', 'formula', {'outputIndex':ZERO, 'operation':'add', 'inputs':[{'value':0},{'value':0}]}),
        ('manual', 'controlGate', {'outputIndex':MANUAL, 'combine':'all',
         'conditions':[{'index':MODE,'operator':'eq','value':2},
                       {'index':POWER,'operator':'gte','value':-124.8},
                       {'index':POWER,'operator':'lte','value':124.8}]}),
        ('direction', 'switch', {'outputIndex':DIRECTION, 'leftIndex':POWER,
         'operator':'gt', 'rightValue':0, 'trueValue':2, 'falseValue':1}),
        ('nonzero', 'switch', {'outputIndex':NONZERO, 'leftIndex':POWER,
         'operator':'ne', 'rightValue':0, 'trueIndex':DIRECTION, 'falseValue':0}),
        ('permit', 'controlGate', {'outputIndex':PERMIT, 'combine':'any',
         'conditions':[{'index':MODE, 'operator':'eq', 'value':1},
                       {'index':MANUAL, 'operator':'eq', 'value':1}]}),
        ('phase', 'switch', {'outputIndex':PHASE, 'conditionIndex':MANUAL,
         'trueIndex':NONZERO, 'falseIndex':400017}),
    ]
    for number, (name, kind, params) in enumerate(definitions):
        graph['nodes'].append({'id':'local_control_'+name, 'type':kind,
            'typeVersion':'1.0.0', 'displayName':'本地控制 '+name, 'enabled':True,
            'order':0, 'groupId':'', 'parameters':params, 'ports':[],
            'layout':{'x':20+number*300,'y':20,'width':280,'height':180}})
    dependencies = [('zero','power_arbiter'), ('manual','power_arbiter'), ('permit','cycle_basic_safety_gate'),
                    ('manual','local_control_override'),('remote','local_control_override'),
                    ('manual','local_control_override_p'),('manual','local_control_override_q'),
                    ('override','power_arbiter'),('override_p','power_arbiter'),('override_q','power_arbiter'),
                    ('phase','cycle_charge_direction_allowed'),
                    ('phase','cycle_discharge_direction_allowed'),
                    ('manual','local_control_phase'), ('manual','local_control_permit'),
                    ('direction','local_control_nonzero'),
                    ('nonzero','local_control_phase')]
    dependencies += [('force_full',n) for n in ['force_full_soc_upper']+
        [f'force_full_{kind}_{i}' for kind in ('active','reactive') for i in range(3)]]
    for i, (source_id, target) in enumerate(dependencies):
        graph['links'].append({'id':f'local_control_dep_{i}', 'kind':'dependency',
            'fromNodeId':'local_control_'+source_id, 'toNodeId':target,
            'fromPortId':'','toPortId':'','inferred':False})
    graph['links'].append({'id':'local_control_dep_cycle', 'kind':'dependency',
        'fromNodeId':'cycle_phase_sequence','toNodeId':'local_control_phase',
        'fromPortId':'','toPortId':'','inferred':False})
    force_sources=[n['id'] for n in source['nodes'] if any(
        p['direction']=='output' and p.get('binding',{}).get('index')==400030 for p in n['ports'])]
    if len(force_sources)!=1: raise ValueError('Force-full flag producer is ambiguous')
    graph['links'].append({'id':'local_control_dep_force_source','kind':'dependency',
        'fromNodeId':force_sources[0],'toNodeId':'local_control_force_full',
        'fromPortId':'','toPortId':'','inferred':False})
    graph['compile']['maxNodes']=max(graph['compile']['maxNodes'],len(graph['nodes']))
    return graph


def prepare(source, destination, password):
    if not password:
        raise ValueError('A local control password is required')
    identity = read(source/'runtime/device_identity.json')
    if identity['machineCode'] != 'COMM202600103':
        raise ValueError('Only the audited COMM202600103 project is supported')
    if destination.exists():
        raise ValueError('Output must be a new directory')
    graph = read(source/'runtime/logic/shuntong_ems_graph.json')
    devices = [read(p) for p in (source/'runtime/devices').glob('*.json')]
    used = {p['index'] for d in devices for m in d.get('meters',[]) for p in m.get('points',[])}
    def indexes(value):
        if isinstance(value, dict):
            for key, item in value.items():
                if key.lower().endswith('index') and isinstance(item, int): used.add(item)
                elif key.lower().endswith('indexes') and isinstance(item, list): used.update(item)
                else: indexes(item)
        elif isinstance(value, list):
            for item in value: indexes(item)
    indexes(graph)
    if used.intersection(range(MODE, OVERRIDE_Q+1)):
        raise ValueError('Reserved control indices collide')
    result = migrate_graph(graph)
    shutil.copytree(source, destination)
    write(destination/'runtime/logic/shuntong_ems_graph.json', result)
    virtual_path = destination/'runtime/devices/device_ems_virtual.json'
    virtual = read(virtual_path)
    meter = next(m for m in virtual['meters'] if m['meterCode']=='EMS_CORE')
    specs = [(MODE,'local_control_mode','控制模式：0暂停归零 / 1自动策略 / 2手动功率',0,2,1,0,''),
             (POWER,'local_manual_total_kw','手动总有功：正充负放',-124.8,124.8,0.1,0,'kW')]
    for index, code, name, low, high, step, initial, unit in specs:
        point = {'index':index,'pointCode':code,'name':name,'enabled':True,
                 'initialValue':initial,'fullUpload':True,'reportOnChange':True,
                 'read':{'enable':False,'dataType':'float64','unit':unit},
                 'write':{'enable':True,'dataType':'float64','min':low,'max':high,'step':step}}
        if index==MODE: point['write']['allowedValues']=[0,1,2]
        meter['points'].append(point)
    feedback=[(APPLIED,'local_control_applied','计算侧已读取模式'),
              (700345,'local_final_active_a','A相安全门后指令'),
              (700346,'local_final_active_b','B相安全门后指令'),
              (700347,'local_final_active_c','C相安全门后指令')]
    for index,code,name in feedback:
        if not any(p['index']==index for p in meter['points']):
            meter['points'].append({'index':index,'pointCode':code,'name':name,'enabled':True,
                'read':{'enable':False,'dataType':'float64','unit':'' if index==APPLIED else 'kW'},'write':{'enable':False}})
    for index in range(MANUAL,OVERRIDE_Q+1):
        if not any(p['index']==index for p in meter['points']):
            meter['points'].append({'index':index,'pointCode':'local_control_internal_'+str(index),
                'name':'本地控制内部计算量 '+str(index),'enabled':True,
                'read':{'enable':False,'dataType':'float64'},'write':{'enable':False}})
    write(virtual_path,virtual)
    scada=destination/'scada'
    tags, routes=read(scada/'tags.json'),read(scada/'runtime-map.json')
    for index, code, name, low, high, step, initial, unit in specs:
        tag='EMS_CORE.'+code
        tags.append({'tagId':tag,'nodeId':'edge-1','deviceId':'EMS_CORE','meterCode':'EMS_CORE',
            'pointCode':code,'displayName':name,'unit':unit,'dataType':'float64',
            'access':'readWrite','indexFallback':index})
        routes.append({'nodeId':'edge-1','tagId':tag,'sharedMemoryName':'gateway_point_store_ems_virtual',
            'index':index,'writable':True,'dataType':'float64','unit':unit})
    for index,code,name in feedback:
        tag='EMS_CORE.'+code
        unit='' if index==APPLIED else 'kW'
        tags.append({'tagId':tag,'nodeId':'edge-1','deviceId':'EMS_CORE','meterCode':'EMS_CORE',
            'pointCode':code,'displayName':name,'unit':unit,'dataType':'float64','access':'read','indexFallback':index})
        routes.append({'nodeId':'edge-1','tagId':tag,'sharedMemoryName':'gateway_point_store_ems_virtual',
            'index':index,'writable':False,'dataType':'float64','unit':unit})
    write(scada/'tags.json',tags); write(scada/'runtime-map.json',routes)
    def widget(id,title,x,y,width=540,kind='qtButton',tag=None,value=None,target=None):
        action=None
        if target: action={'type':'navigate','targetScreen':target}
        elif kind=='qtButton': action={'type':'writeSetpoint','nodeId':'edge-1',
            'tagId':tag,'value':'' if value is None else str(value),'requiresConfirmation':True,'highPriority':False}
        return {'widgetId':id,'type':kind,'title':title,'geometry':{'x':x,'y':y,'width':width,'height':88},
            'zIndex':1000,'visible':True,'bindings':[] if not tag else [{'nodeId':'edge-1','tagId':tag,'slot':'value'}],
            'stateRules':[],'action':action,'properties':{'qtText':title,'qtFontSize':28,
            'qtTextColor':'#FFFFFF','qtBackgroundColor':'#243B42','qtTransparent':False}}
    mode='EMS_CORE.local_control_mode'; power='EMS_CORE.local_manual_total_kw'
    widgets=[widget('back','返回运行模式',40,30,300,target='RunMode'),
        widget('auto','启用自动策略',40,180,tag=mode,value=1),
        widget('pause','暂停策略并归零',640,180,tag=mode,value=0),
        widget('manual','启用手动功率',1240,180,tag=mode,value=2),
        widget('power','手动总有功 (kW，正充负放)',40,320,kind='qtInput',tag=power),
        widget('mode-value','控制模式设置',640,320,kind='valueCard',tag=mode),
        widget('power-value','手动总有功设定',1240,320,kind='valueCard',tag=power)]
    widgets[5]['properties']['valueMapJson']=json.dumps({'0':'暂停并归零','1':'自动策略','2':'手动功率'},ensure_ascii=False)
    widgets[3]['properties'].update(qtTextAlignment='Center',qtVerticalAlignment='Center')
    input_label=widget('power-label','手动总有功 (kW，正充负放)',40,282,540,kind='qtText')
    input_label['geometry']['height']=32
    input_label['properties'].update(qtFontSize=24,qtTransparent=True,qtVerticalAlignment='Center')
    widgets.append(input_label)
    for i,index in enumerate([700345,700346,700347,1230]):
        candidates=[r for r in routes if r['index']==index]
        if len(candidates)!=1: raise ValueError(f'Feedback route missing/ambiguous: {index}')
        widgets.append(widget('feedback-'+str(index),['A相最终指令','B相最终指令','C相最终指令','PCS实际总有功'][i],
            40+i*465,520,430,kind='valueCard',tag=candidates[0]['tagId']))
    applied=widget('applied-mode','计算侧已读取模式',40,700,540,kind='valueCard',tag='EMS_CORE.local_control_applied')
    applied['properties']['valueMapJson']=widgets[5]['properties']['valueMapJson']
    widgets.append(applied)
    widgets.append(widget('scan-period','策略扫描周期：10 秒',640,700,540,kind='qtText'))
    widgets.append(widget('limit','当前策略限幅：每相 30 kW',1240,700,620,kind='qtText'))
    widgets.append(widget('rated-limit','硬件写入上限：每相 41.6 kW / 总有功 124.8 kW',40,840,1780,kind='qtText'))
    write(scada/'screens/LocalControl.json',{'screenId':'LocalControl','title':'PCS功率与策略控制',
        'width':1920,'height':1080,'background':'#17272C','widgets':widgets})
    for page in ['PcsMon','RunMode','ChargeParam']:
        path=scada/'screens'/f'{page}.json'; screen=read(path)
        if page=='PcsMon':
            buttons=[w for w in screen['widgets'] if w['widgetId'] in ('qt_PcsBtnSetKw','qt_PcsBtnSetKvar')]
            if len(buttons)!=2: raise ValueError('Expected PCS control entries not found')
            for button in buttons:
                button['action']={'type':'navigate','targetScreen':'LocalControl'}
                button['bindings']=[]
                button['properties']['buttonAction']='navigate'
                button['title']='PCS功率 / 策略控制'
                button['properties']['qtText']=button['title']
        else:
            screen['widgets'].append(widget('local-controls-entry','PCS功率 / 策略控制',1340,970,540,target='LocalControl'))
        write(path,screen)
    manifest=read(scada/'manifest.json'); manifest['packageVersion']='1.0.18-local-control-candidate'
    write(scada/'manifest.json',manifest)
    permissions=read(scada/'permissions.json')
    access=permissions.setdefault('localAccess',{'sessionTimeoutSeconds':900,'users':[], 'protectedScreenPrefixes':[]})
    if not access['users']:
        salt=secrets.token_hex(16)
        access['users'].append({'username':'operator','salt':salt,
            'passwordSha256':hashlib.sha256((salt+':'+password).encode()).hexdigest(),'roles':['operator']})
    protected=set(access.get('protectedScreenPrefixes',[]))
    protected.add('LocalControl')
    # The existing loader requires every writable screen to be protected once local access is enabled.
    for path in (scada/'screens').glob('*.json'):
        page=read(path)
        if any((w.get('action') or {}).get('type') in ('writeSetpoint','toggle','pulse')
               for w in page['widgets']):
            protected.add(page['screenId'])
    access['protectedScreenPrefixes']=sorted(protected)
    permissions['roles']=sorted(set(permissions.get('roles',[]))|{'operator'})
    write(scada/'permissions.json',permissions)
    # Regenerate only the checksum format already used by the project.
    checksum=read(scada/'checksums.json')
    if not isinstance(checksum,dict): raise ValueError('Unsupported checksums format')
    write(scada/'checksums.json',{p.relative_to(scada).as_posix():hashlib.sha256(p.read_bytes()).hexdigest()
        for p in sorted(scada.rglob('*')) if p.is_file() and p.name!='checksums.json'})
    write(destination/'control-migration.json',{'status':'candidate-not-deployed','machineCode':identity['machineCode'],
        'modeIndex':MODE,'manualPowerIndex':POWER,'maximumTotalKw':124.8,
        'scanIntervalMs':10000,'missingModeDefault':0,'requires':['C++ graph loader regression','render verification',
        'zero-output and SOC simulation','deployment approval'],
        'sourceHashes':{str(p.relative_to(source)):hashlib.sha256(p.read_bytes()).hexdigest()
          for p in source.rglob('*.json')}})


if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('source',type=Path);parser.add_argument('destination',type=Path)
    args=parser.parse_args();prepare(args.source,args.destination,os.environ.get('SCADA_OPERATOR_PASSWORD',''))
