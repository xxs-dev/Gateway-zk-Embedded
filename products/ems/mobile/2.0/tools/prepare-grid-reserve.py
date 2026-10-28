"""Prepare audited, offline COMM202600104 graph/point fragments; never deploy.

Inputs: an individually audited V2 graph and its runtime directory. Historical
inputs produce UI/load fragments; a current snapshot preserves its runtime tree.
Current snapshots require an explicit initial mode; this tool never deploys.
"""
import argparse
import copy
import hashlib
import json
import math
import shutil
from pathlib import Path

BASELINE_SHA256 = 'f73ad91cfc1fec4755af895413d83a18a22edcb299361bbb85e069df197846d0'
BASELINE_SOURCE = ('Gateway-zk-cleanup-backup-20260806-225840/generated/artifacts/'
                   'voltage-pv-refactor-20260806161736/'
                   'shuntong_ems_modular_graph.COMM202600104.json')
CURRENT_SHA256 = '621200c11008125dc1b1e0b531bfa099fad07128eed7f7ab2695495b59ddcdf3'
CURRENT_GRAPH = 'logic/shuntong_ems_cycle_2kw.active.json'
RUNTIME_PREFIX = '/opt/modbus-gateway/config/runtime/'
MODE = 740100
MANUAL = [740101, 740102, 740103]
STATUS, APPLIED = 740104, 740105
FINAL = [740106, 740107, 740108]
PHASE_POWER = 740109
FIRST_INTERNAL, LAST_INTERNAL = 740110, 740199


def read(path):
    def reject(value):
        raise ValueError('Non-finite JSON number: ' + value)
    return json.loads(path.read_text(encoding='utf-8-sig'), parse_constant=reject)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False)
                    + '\n', encoding='utf-8')


def point_indexes(value):
    """Include V2 ports and list-valued parameters in collision detection."""
    result = set()
    if isinstance(value, dict):
        for key, item in value.items():
            if (key.lower().endswith('index') or key.endswith('Output')) and type(item) is int:
                result.add(item)
            elif key.lower().endswith('indexes') and isinstance(item, list):
                result.update(x for x in item if type(x) is int)
            result.update(point_indexes(item))
    elif isinstance(value, list):
        for item in value:
            result.update(point_indexes(item))
    return result


def validate_points(devices):
    points = {}
    for device in devices:
        for meter in device.get('meters', []):
            for point in meter.get('points', []):
                index = point['index']
                if index in points:
                    raise ValueError(f'Duplicate point index: {index}')
                points[index] = point
    # Physical read registers, not command aliases or virtual defaults.
    for index, address, function, scale in [(1569, 8206, 4, .1), (1216, 88, 2, 1)]:
        p = points.get(index, {})
        r = p.get('read', {})
        if (not p.get('enabled') or not r.get('enable') or p.get('write', {}).get('enable')
                or p.get('address') != address or r.get('function') != function
                or r.get('scale') != scale):
            raise ValueError(f'Unverified physical feedback: {index}')
    if points[1216].get('valueMap') != {'0': '\u672a\u5e76\u7f51', '1': '\u5e76\u7f51'}:
        raise ValueError('Unverified grid feedback enumeration')
    stop = points.get(1202, {})
    if not stop.get('enabled') or stop.get('address') != 3:
        raise ValueError('Unverified PCS stop coil: 1202')
    for access, function in [('read', 1), ('write', 5)]:
        spec = stop.get(access, {})
        if (not spec.get('enable') or spec.get('function') != function
                or spec.get('address', stop.get('address')) != 3
                or spec.get('length') != 1 or spec.get('dataType') != 'bit'
                or spec.get('scale') != 1 or spec.get('offset') != 0):
            raise ValueError('Unverified PCS stop coil read/write semantics: 1202')
    for index in [1399, 3999, 1212, 1215, 1214, 1462, 8199, 1279, 1556, 1557]:
        if index not in points or not points[index].get('enabled'):
            raise ValueError(f'Missing safety point: {index}')
    for phase in range(6):
        p = points.get(1318+phase, {})
        w = p.get('write', {})
        if (not p.get('enabled') or p.get('address') != 309+phase
                or not w.get('enable') or w.get('function') != 6
                or w.get('scale') != .1 or w.get('min') != -41.6 or w.get('max') != 41.6):
            raise ValueError('Unverified PCS power write mapping/rating')
    return points


def migrate_graph(source, max_phase_kw):
    if not math.isfinite(max_phase_kw) or not 1 <= max_phase_kw <= 41.6:
        raise ValueError('Require 1 <= maxPhaseKw <= 41.6 (default request is 1 kW)')
    if point_indexes(source).intersection(range(MODE, LAST_INTERNAL + 1)):
        raise ValueError('Reserved graph indices collide')
    graph = copy.deepcopy(source)
    nodes = {n['id']: n for n in graph['nodes']}
    if len(nodes) != len(graph['nodes']):
        raise ValueError('Duplicate graph node ids')
    for i in range(6):
        p = nodes[f'cycle_pcs_control_write_{i}']['parameters']
        if (p['targetIndex'], p['minValue'], p['maxValue'], p['permitIndex']) != (1318+i, -41.6, 41.6, 1399):
            raise ValueError('Unexpected hardware writer/ownership path')
    next_index = FIRST_INTERNAL
    generated = []

    def add(name, kind, p, output=None):
        nonlocal next_index
        if output is None:
            output = next_index
            next_index += 1
        p = copy.deepcopy(p)
        if kind == 'sequence':
            p['stateOutputIndex'] = output
        elif kind != 'gridReserve':
            p['outputIndex'] = output
        labels = {'auto': '原自动模式', 'manual': '手动模式及功率范围校验',
            'enabled': '并网保电模式', 'bms_safe': '电池工作指令安全校验',
            'device_safety': '设备安全允许', 'charge_command': '电池充电指令允许',
            'discharge_command': '电池放电指令允许', 'charge_permit': '充电安全允许',
            'discharge_permit': '放电安全允许', 'controller': '并网保电控制器',
            'manual_charge': '手动充电及荷电状态允许', 'manual_discharge': '手动放电及荷电状态允许',
            'manual_run': '手动运行允许', 'local_run': '本地运行或安全待机请求',
            'run_permit': '原自动与本地运行仲裁', 'original_capacity_reserve': '原自动容量预留选择',
            'soc': '保电真实荷电状态选择', 'soc_upper': '保电满充上限选择',
            'applied_local': '本地模式反馈', 'valid_local': '本地模式有效校验',
            'applied_valid': '本地模式应用确认', 'applied': '已应用控制模式',
            'stop_latch': '策略暂停状态', 'stop_clear': '策略允许启机'}
        prefixes = {'positive': '手动充电方向判断', 'manual_direction': '手动方向安全选择',
            'manual_safe': '手动功率安全门', 'manual_power': '手动模式功率选择',
            'local_power': '手动与保电功率选择', 'power': '原自动与本地有功仲裁',
            'reactive': '原自动与本地无功仲裁', 'final': '最终安全门后有功指令'}
        label = labels.get(name)
        if label is None:
            prefix, phase = name.rsplit('_', 1)
            label = 'ABC'[int(phase)] + '相' + prefixes[prefix]
        node = {'id': 'grid_reserve_' + name, 'type': kind, 'typeVersion': '1.0.0',
                'displayName': label, 'enabled': True, 'order': 0,
                'groupId': '', 'parameters': p, 'ports': [],
                'layout': {'x': 20, 'y': 20 + len(generated)*200, 'width': 280, 'height': 180}}
        generated.append(node)
        graph['nodes'].append(node)
        return output

    def condition(index, operator='eq', value=1):
        return {'index': index, 'operator': operator, 'value': value}

    def gate(name, conditions, combine='all'):
        return add(name, 'controlGate', {'combine': combine, 'conditions': conditions})

    def select(name, selector, yes, no, output=None):
        return add(name, 'switch', {'conditionIndex': selector, **yes, **no}, output)

    auto = gate('auto', [condition(MODE, value=1)])
    manual = gate('manual', [condition(MODE, value=2)] +
                  [condition(i, op, v) for i in MANUAL
                   for op, v in [('gte', -max_phase_kw), ('lte', max_phase_kw)]])
    reserve = gate('enabled', [condition(MODE, value=3)])
    # A retained stop coil is equipment state, not operator intent. The UI requires
    # requested/applied pause before manual stop; re-enabling a mode grants startup.
    # Keep the existing output slot so previously exported bindings remain stable.
    stop_latch = add('stop_latch', 'sequence', {'initialState': 0, 'evaluateOnInitialize': True,
        'states': [{'id': 0, 'name': '允许策略启机'}, {'id': 1, 'name': '策略已暂停'}],
        'transitions': [{'from': 0, 'to': 1, 'conditions': [condition(MODE, value=0)]},
                        {'from': 1, 'to': 0, 'conditions': [condition(MODE, value=2)]},
                        {'from': 1, 'to': 0, 'conditions': [condition(MODE, value=3)]}]})
    stop_clear = gate('stop_clear', [condition(stop_latch, value=0)])
    # Copy hard device conditions, explicitly separating ordinary charge inhibit.
    hard = copy.deepcopy(nodes['cycle_basic_safety_gate']['parameters']['conditions'])
    hard = [c for c in hard if c['index'] != 1216]
    hard += [condition(3999), condition(1462, value=0), condition(8199, value=0)]
    bms_safe = gate('bms_safe', [condition(1279, value=v)
                    for v in [4369, 8738, 21845, 48059, 52428]], 'any')
    hard += [condition(bms_safe), condition(stop_clear)]
    safety = gate('device_safety', hard)
    # Original command gates occur AFTER constraints in the imported scan chain.
    # Evaluate their same hardware-only predicates here without moving old nodes.
    command_charge = gate('charge_command', copy.deepcopy(
        nodes['cycle_charge_command_allowed']['parameters']['conditions']), 'any')
    command_discharge = gate('discharge_command', copy.deepcopy(
        nodes['cycle_discharge_command_allowed']['parameters']['conditions']), 'any')
    charge = gate('charge_permit', [condition(safety), condition(1556, 'gt', 0),
                    condition(command_charge)])
    discharge = gate('discharge_permit', [condition(safety), condition(1557, 'gt', 0),
                    condition(command_discharge)])
    pa, pb, pc, standby = range(next_index, next_index+4)
    next_index += 4
    add('controller', 'gridReserve', {
        'modeIndex': MODE, 'reserveMode': 3, 'socIndex': 1569,
        'gridConnectedIndex': 1216, 'safetyPermitIndex': safety,
        'chargePermitIndex': charge, 'phasePowerIndex': PHASE_POWER,
        'phasePowerKw': 1, 'maxPhaseKw': max_phase_kw, 'restartSoc': 99, 'fullSoc': 100,
        'paOutput': pa, 'pbOutput': pb, 'pcOutput': pc,
        'statusOutput': STATUS, 'standbyOutput': standby}, output=STATUS)
    manual_charge = gate('manual_charge', [condition(charge), condition(1569, 'lt', 95)])
    manual_discharge = gate('manual_discharge', [condition(discharge), condition(1569, 'gt', 20)])
    manual_run = gate('manual_run', [condition(manual), condition(safety), condition(1216)])
    local_run = gate('local_run', [condition(manual_run), condition(standby)], 'any')
    old_direction = nodes['cycle_direction_allowed']['parameters']['outputIndex']
    direction = select('run_permit', auto, {'trueIndex': old_direction}, {'falseIndex': local_run})
    # Keep the original sequence and controlWrite nodes, including ownership flags.
    for node_id in ['cycle_start_hold_sequence', 'cycle_start_hold_value',
                    'cycle_phase_control_mode', 'cycle_stop_clear', 'cycle_start',
                    'cycle_runtime_safety_gate']:
        def replace(value):
            if isinstance(value, dict):
                for key, item in value.items():
                    if key in ('index', 'permitIndex') and item == old_direction:
                        value[key] = direction
                    else:
                        replace(item)
            elif isinstance(value, list):
                for item in value:
                    replace(item)
        replace(nodes[node_id]['parameters'])
    # SOC95 remains the automatic limit; reserve uses actual SOC and 100%.
    constraints = nodes['power_constraints']['parameters']
    constraints['reserveEnableIndex'] = select('original_capacity_reserve', auto,
        {'trueIndex': constraints['reserveEnableIndex']}, {'falseValue': 0})
    constraints['stateIndex'] = select('soc', reserve, {'trueIndex': 1569},
                                      {'falseIndex': constraints['stateIndex']})
    constraints['stateUpperIndex'] = select('soc_upper', reserve, {'trueValue': 100},
                                           {'falseIndex': constraints['stateUpperIndex']})
    for phase, reserve_power in enumerate([pa, pb, pc]):
        sign = gate(f'positive_{phase}', [condition(MANUAL[phase], 'gt', 0)])
        permit = select(f'manual_direction_{phase}', sign, {'trueIndex': manual_charge},
                        {'falseIndex': manual_discharge})
        safe_manual = select(f'manual_safe_{phase}', permit, {'trueIndex': MANUAL[phase]}, {'falseValue': 0})
        manual_power = select(f'manual_power_{phase}', manual, {'trueIndex': safe_manual}, {'falseValue': 0})
        local_power = select(f'local_power_{phase}', reserve, {'trueIndex': reserve_power}, {'falseIndex': manual_power})
        constraints['activeInputIndexes'][phase] = select(f'power_{phase}', auto,
            {'trueIndex': constraints['activeInputIndexes'][phase]}, {'falseIndex': local_power})
        constraints['reactiveInputIndexes'][phase] = select(f'reactive_{phase}', auto,
            {'trueIndex': constraints['reactiveInputIndexes'][phase]}, {'falseValue': 0})
        # Feedback means final safety-gated command, not PCS measured power/ACK.
        select(f'final_{phase}', auto, {'trueIndex': nodes[f'cycle_safe_p{phase}']['parameters']['outputIndex']},
               {'falseIndex': nodes[f'cycle_safe_p{phase}']['parameters']['outputIndex']}, FINAL[phase])
    applied_local = select('applied_local', reserve, {'trueValue': 3}, {'falseValue': 2})
    valid_local = gate('valid_local', [condition(manual), condition(reserve)], 'any')
    # A normalized accepted mode, independent of whether a safety interlock is active.
    local_applied = select('applied_valid', valid_local, {'trueIndex': applied_local}, {'falseValue': 0})
    select('applied', auto, {'trueValue': 1}, {'falseIndex': local_applied}, APPLIED)
    # Energy-saving stop is not a protection. Full reserve must remain online.
    stop = nodes['pcs_energy_stop_permit']
    stop['parameters']['conditions'].append(condition(auto))
    position = len(stop['parameters']['conditions']) - 1
    stop['ports'].append({'id': 'grid_reserve_auto_only', 'displayName': '仅原自动策略允许节能停机',
        'direction': 'input', 'valueType': 'number', 'unit': '', 'required': True,
        'runtimePath': f'/conditions/{position}/index', 'binding': {'kind': 'point', 'index': auto}})
    if next_index > LAST_INTERNAL + 1:
        raise ValueError('Internal index allocation exhausted')
    # Refresh imported ports after rewiring and create ports for all new nodes.
    for node in graph['nodes']:
        p = node['parameters']
        for port in node['ports']:
            value = p
            for part in port['runtimePath'].strip('/').split('/'):
                value = value[int(part)] if isinstance(value, list) else value[part]
            if port.get('binding', {}).get('kind') == 'point':
                port['binding']['index'] = value
    for node in generated:
        def ports(value, path=''):
            if isinstance(value, dict):
                for key, item in value.items():
                    pointer = path + '/' + key
                    if type(item) is int and (key.lower().endswith('index') or key.endswith('Output')):
                        output = key in ('outputIndex', 'stateOutputIndex') or key.endswith('Output')
                        node['ports'].append({'id': pointer.strip('/').replace('/', '_'),
                            'displayName': '输出点' if output else '输入点', 'direction': 'output' if output else 'input',
                            'valueType': 'number', 'unit': '', 'required': True, 'runtimePath': pointer,
                            'binding': {'kind': 'point', 'index': item}})
                    else:
                        ports(item, pointer)
            elif isinstance(value, list):
                for i, item in enumerate(value):
                    ports(item, path + '/' + str(i))
        ports(node['parameters'])
    # Imported graphs intentionally omit some feedback wires (previous scan).
    # Only rewire changed input bindings, never infer the entire imported graph.
    old_inputs = {(n['id'], p['id']): p.get('binding', {}).get('index')
                  for n in source['nodes'] for p in n['ports'] if p['direction'] == 'input'}
    changed_inputs = {(n['id'], p['id']) for n in graph['nodes'] for p in n['ports']
                      if p['direction'] == 'input' and
                      old_inputs.get((n['id'], p['id'])) != p.get('binding', {}).get('index')}
    graph['links'] = [link for link in graph['links'] if link['kind'] != 'data' or
                      (link['toNodeId'], link['toPortId']) not in changed_inputs]
    producers = {}
    for node in graph['nodes']:
        for port in node['ports']:
            if port['direction'] == 'output' and port.get('binding', {}).get('kind') == 'point':
                producers.setdefault(port['binding']['index'], []).append((node['id'], port['id']))
    for node in graph['nodes']:
        for port in node['ports']:
            if (node['id'], port['id']) not in changed_inputs:
                continue
            for source_id, source_port in producers.get(port.get('binding', {}).get('index'), []):
                if source_id != node['id']:
                    graph['links'].append({'id': 'grid_reserve_wire_' + str(len(graph['links'])),
                        'kind': 'data', 'fromNodeId': source_id, 'toNodeId': node['id'],
                        'fromPortId': source_port, 'toPortId': port['id'], 'inferred': True})
    graph['compile']['maxNodes'] = max(graph['compile']['maxNodes'], len(graph['nodes']))
    graph['compile']['maxEdges'] = max(graph['compile']['maxEdges'], len(graph['links']) + 64)
    return graph


def current_rule(runtime, graph_path):
    if graph_path.resolve() != (runtime/CURRENT_GRAPH).resolve():
        raise ValueError('Current graph must be the captured active runtime file')
    app = read(runtime/'apps/mqtt-service.json')
    compute = app.get('computeEngine', {})
    if compute.get('enabled') is not True:
        raise ValueError('Current compute engine is not enabled; no implicit activation')
    rules = [r for r in compute.get('rules', []) if r.get('enabled') is True and
             r.get('script', {}).get('graphFile') == RUNTIME_PREFIX + CURRENT_GRAPH]
    if len(rules) != 1:
        raise ValueError('Expected one enabled rule for the captured active graph')
    rule = rules[0]
    if (rule.get('ruleCode') != 'charge_discharge_cycle_8kw'
            or rule.get('trigger') != {'type': 'interval', 'intervalMs': 1000}
            or rule['script'].get('type') != 'graphEms'
            or rule['script'].get('graphProfile', {}).get('CHARGE_DISCHARGE_TEST') != '1'):
        raise ValueError('Current active rule/profile changed; review required')
    configured = {RUNTIME_PREFIX + p.relative_to(runtime).as_posix()
                  for p in (runtime/'devices').glob('*.json')}
    if set(app.get('deviceConfigFiles', [])) != configured:
        raise ValueError('Current device files do not match the active app')
    # Candidate outputs are strictly private to the selected graph. Check other
    # captured graphs before reserving their index range.
    for path in (runtime/'logic').glob('*.json'):
        if path.resolve() != graph_path.resolve() and point_indexes(read(path)).intersection(range(MODE, LAST_INTERNAL+1)):
            raise ValueError('Reserved index collision in another runtime graph')
    return rule


def prepare(runtime, graph_path, destination, max_phase_kw, initial_mode=None):
    runtime, graph_path, destination = map(Path, (runtime, graph_path, destination))
    if destination.exists() or destination.resolve().is_relative_to(runtime.resolve()):
        raise ValueError('Output must be a new directory outside the source runtime')
    source_sha = digest(graph_path)
    if source_sha not in (BASELINE_SHA256, CURRENT_SHA256):
        raise ValueError('Unaudited graph SHA256; no automatic topology migration')
    current = source_sha == CURRENT_SHA256
    rule = current_rule(runtime, graph_path) if current else None
    if current and initial_mode is None:
        raise ValueError('Current snapshot requires explicit initial mode 0 (pause) or 1 (preserve original automatic)')
    if initial_mode is None:
        initial_mode = 0
    if type(initial_mode) is not int or initial_mode not in (0, 1):
        raise ValueError('Preparation may not activate manual or grid reserve mode')
    identity_path = runtime/'device_identity.json'
    if read(identity_path).get('machineCode') != 'COMM202600104':
        raise ValueError('Expected COMM202600104 identity')
    paths = sorted((runtime/'devices').glob('*.json'))
    devices = [read(path) for path in paths]
    points = validate_points(devices)
    if set(points).intersection(range(MODE, LAST_INTERNAL+1)):
        raise ValueError('Reserved device indices collide')
    graph = migrate_graph(read(graph_path), max_phase_kw)
    specs = [(MODE, 'local_control_mode', 0, 3, initial_mode),
             *[(i, 'local_manual_' + phase + '_kw', -max_phase_kw, max_phase_kw, 0)
               for i, phase in zip(MANUAL, 'abc')],
             (PHASE_POWER, 'grid_reserve_phase_kw', 0, max_phase_kw, 1)]
    additions = []
    names = {MODE: '控制模式：暂停、原自动、手动三相、并网保电',
             PHASE_POWER: '保电每相补电功率', STATUS: '并网保电状态', APPLIED: '已应用控制模式'}
    names.update({i: phase + '相手动有功功率（正充负放）' for i, phase in zip(MANUAL, 'ABC')})
    names.update({i: phase + '相最终安全门后指令' for i, phase in zip(FINAL, 'ABC')})
    for index, code, low, high, initial in specs:
        p = {'index': index, 'pointCode': code, 'name': names[index], 'enabled': True,
             'initialValue': initial, 'fullUpload': True, 'reportOnChange': True,
             'read': {'enable': False, 'dataType': 'float64'},
             'write': {'enable': True, 'dataType': 'float64', 'min': low, 'max': high,
                       'step': 1 if index == MODE else .1}}
        if index == MODE:
            p['write']['allowedValues'] = [0, 1, 2, 3]
        additions.append(p)
    used = point_indexes(graph).intersection(range(MODE, LAST_INTERNAL+1))
    for index in sorted(used - {s[0] for s in specs}):
        additions.append({'index': index, 'pointCode': 'grid_reserve_' + str(index),
            'name': names.get(index, '并网保电内部计算点 ' + str(index)), 'enabled': True, 'initialValue': 0,
            'read': {'enable': False, 'dataType': 'float64'}, 'write': {'enable': False}})
    report = {'status': 'offline-historical-candidate-fragments-not-deployed',
        'machineCode': 'COMM202600104', 'productionCurrentVerified': False,
        'graphEvidence': BASELINE_SOURCE, 'graphSha256': digest(graph_path),
        'sourceHashes': {str(p.relative_to(runtime)): digest(p) for p in [identity_path] + paths},
        'maxPhaseKw': max_phase_kw, 'hardwareWriteLimitKw': 41.6,
        'socIndex': 1569, 'gridConnectedIndex': 1216,
        'modeIndex': MODE, 'statusIndex': STATUS, 'appliedModeIndex': APPLIED,
        'finalCommandIndexes': FINAL, 'phasePowerIndex': PHASE_POWER,
        'requires': ['Merge point fragment into the active EMS virtual meter',
            'Confirm active V2 graph and CHARGE_DISCHARGE_TEST profile offline',
            'Grok acceptance review and C++ graph-load/control simulation',
            'Confirm current physical hardware rating and production baseline before any deployment'],
        'uncertainties': ['Historical automatic branch uses SOC1570; preserved, not silently corrected',
            'Historical point1569 is SOC; point1570 is SOH. Original automatic behavior is preserved',
            'No independently verified BMS full-charge bit in the historical RTU point tables; fullIndex omitted, SOC100-only completion',
            'Historical runtime and V2 graph have different capture dates; not a production snapshot']}
    virtual_meters = [(d, meter) for d in devices for meter in d.get('meters', [])
                      if meter.get('meterCode') == 'EMS_CORE']
    if len(virtual_meters) != 1:
        raise ValueError('Expected exactly one EMS_CORE virtual meter')
    original_nodes = {n['id']: n for n in read(graph_path)['nodes']}
    report['initialMode'] = initial_mode
    report['initialModeMeaning'] = ('inherit-existing-automatic-without-enabling-rules-or-profiles'
                                  if initial_mode == 1 else 'pause-existing-power-control')
    report['factoryConfigurationModified'] = False
    report['factoryDefaultMode'] = 0
    report['candidateOutputScope'] = 'selected-graph-only; no cross-graph or service consumption'
    report['publicationFailure'] = 'shared invalidation best effort; same-graph protective snapshot guarantees downstream zero requests'
    report['modifiedOriginalNodeIds'] = [n['id'] for n in graph['nodes']
        if n['id'] in original_nodes and n != original_nodes[n['id']]]
    if current:
        report.update(status='current-snapshot-incremental-candidate-not-deployed',
            graphEvidence=str(graph_path.resolve()), snapshotCapturedDate='2026-09-12',
            activeGraphFile=CURRENT_GRAPH, activeRuleCode=rule['ruleCode'],
            activeGraphProfile=rule['script']['graphProfile'],
            activeRuleIntervalMs=rule['trigger']['intervalMs'],
            sourceHashes={p.relative_to(runtime).as_posix(): digest(p)
                          for p in sorted(runtime.rglob('*')) if p.is_file()},
            requires=['Grok current-snapshot migration acceptance',
                      'Confirm operator-approved initial mode and live PCS power before switching configuration',
                      'Preserve retained parameters and ownership; do not restore historical runtime',
                      'Current candidate C++ control integration and full graph load',
                      'Deployment remains a separate main-task operation'],
            uncertainties=['Snapshot configuration is not a live power/operating-state reading',
                'No verified BMS full bit; SOC100-only completion with independent charge/safety permission',
                'Mode0 pauses the existing 8kW cycle; mode1 preserves its existing behavior, not a new activation'])
    destination.mkdir(parents=True)
    write(destination/'grid-reserve.graph.json', graph)
    write(destination/'grid-reserve.points.json', additions)
    write(destination/'grid-reserve.migration.json', report)
    # Minimal offline UI/load fixture. Never copy TLS, credentials or service apps.
    fixture = destination/'runtime'
    if current:
        shutil.copytree(runtime, fixture)
    else:
        (fixture/'devices').mkdir(parents=True)
        (fixture/'logic').mkdir()
        write(fixture/'device_identity.json', {'schemaVersion': '1.0.0', 'machineCode': 'COMM202600104'})
    virtual_meters[0][1]['points'].extend(additions)
    for path, device in zip(paths, devices):
        if not current or device is virtual_meters[0][0]:
            write(fixture/'devices'/path.name, device)
    write(fixture/(CURRENT_GRAPH if current else 'logic/shuntong_ems_graph.json'), graph)
    write(fixture/'OFFLINE-CANDIDATE.json', report)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime', required=True, type=Path)
    parser.add_argument('--graph', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--max-phase-kw', required=True, type=float)
    parser.add_argument('--initial-mode', type=int, choices=[0, 1],
                        help='0 pauses existing control; 1 preserves original automatic; mandatory for current snapshots')
    args = parser.parse_args()
    prepare(args.runtime, args.graph, args.output, args.max_phase_kw, args.initial_mode)


if __name__ == '__main__':
    main()
