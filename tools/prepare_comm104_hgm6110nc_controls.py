"""Add HGM6110NC one-shot controls to an explicitly supplied live-derived project."""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import shutil

METER = 'COMM202600104_GENERATOR'
COMMANDS = (
    ('start', '开机', 0, 'gen_fc03_0034', 9, '可能起动发动机。起动和暖机可能超过10秒，未确认时请观察实际状态，不要重复下发。'),
    ('stop', '停机', 1, 'gen_fc03_0034', 0, '执行控制器停机流程，不是急停。散热和停稳可能超过10秒，未确认时请观察实际状态。'),
    ('test', '试机', 2, 'gen_fc01_0040', 1, '试机模式可能起动发动机并发电，请先确认现场安全。'),
    ('auto', '自动', 3, 'gen_fc01_0041', 1, '切换自动模式后，控制器可能根据输入条件自动起动。'),
    ('manual', '手动', 4, 'gen_fc01_0042', 1, '切换控制器手动模式，不改变EMS远程/就地权限。'),
    ('open', '发电分闸', 5, None, None, 'HGM6110发电分闸，可能中断供电。协议无独立位置反馈，驱动回执不代表触点已断开。'),
    ('close', '发电合闸', 6, None, None, 'HGM6110发电合闸，可能使线路带电。请确认现场联锁及接线；驱动回执不代表触点已闭合。'),
)


def load(path):
    return json.loads(Path(path).read_text(encoding='utf-8-sig'))


def save(path, value):
    Path(path).write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')


def sha(path):
    with Path(path).open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def control_points():
    return [dict(index=160200 + address, pointCode='gen_cmd_' + name,
                 name='发电机' + label, category='control', address=address,
                 enabled=True, isStore=False, fullUpload=False, reportOnChange=False,
                 read=dict(enable=False, function=1, length=1, dataType='bool'),
                 write=dict(enable=True, function=5, address=address, length=1,
                            dataType='bool', scale=1, offset=0, byteOrder='AB',
                            allowedValues=[1], min=1, max=1,
                            verifyAfterWrite=False, verifyByRead=False), alarms=[])
            for name, label, address, _, _, _ in COMMANDS]


def bind_controls(config, tags, mappings, page):
    config, tags, mappings, page = copy.deepcopy((config, tags, mappings, page))
    assert len(config['meters']) == 1 and config['meters'][0]['meterCode'] == METER
    assert config['meters'][0]['model'] == 'HGM6110NC'
    assert config['protocol']['slave'] == 1
    points = config['meters'][0]['points']
    additions = control_points()
    assert not {p['index'] for p in additions} & ({p['index'] for p in points} | {m['index'] for m in mappings})
    assert not {p['pointCode'] for p in additions} & {p['pointCode'] for p in points}
    points.extend(additions)
    for p in additions:
        tag = METER + '.' + p['pointCode']
        tags.append(dict(tagId=tag, nodeId='edge-1', deviceId=METER, meterCode=METER,
                         pointCode=p['pointCode'], semanticRole='', displayName=p['name'],
                         unit='', dataType='bool', access='write', indexFallback=p['index']))
        mappings.append(dict(nodeId='edge-1', tagId=tag, sharedMemoryName='gateway_point_store_ttySP6',
                             index=p['index'], writable=True, dataType='bool', unit=''))
    widgets = {w['widgetId']: w for w in page['widgets']}
    widgets['control-mode-heading']['title'] = '发电机控制'
    widgets['control-mode-heading']['properties']['qtText'] = '发电机控制'
    widgets['diesel-empty']['geometry'].update(x=1010, y=248, width=350)
    widgets['control-switch-mode']['visible'] = False
    label = copy.deepcopy(widgets['control-mode-title'])
    label.update(widgetId='gen-running-caption', title='运行状态：')
    label['properties']['qtText'] = '运行状态：'
    label['geometry'].update(x=730, y=289, width=120, height=26)
    state = copy.deepcopy(widgets['control-mode-value'])
    state.update(widgetId='gen-running-feedback', geometry=dict(x=854, y=289, width=220, height=26))
    state_tag = METER + '.gen_fc03_0034'
    state['bindings'] = [dict(nodeId='edge-1', tagId=state_tag, slot='value')]
    running = next(p for p in points if p['pointCode'] == 'gen_fc03_0034')
    state['stateRules'] = [dict(code='gen-running-'+raw, label=label, color='#303438', image='',
        priority=int(raw)+1, match='all', conditions=[dict(nodeId='edge-1', tagId=state_tag,
        comparison='eq', value=raw)]) for raw, label in running['valueMap'].items()]
    page['widgets'].extend([label, state])
    rows = {'start': (400, 380), 'stop': (680, 380), 'test': (960, 480),
            'auto': (400, 480), 'manual': (680, 480), 'open': (400, 580), 'close': (680, 580)}
    for label, y in [('运行控制', 380), ('模式选择', 480), ('发电开关', 580)]:
        w = copy.deepcopy(widgets['control-mode-title'])
        w.update(widgetId='gen-group-' + str(y), title=label)
        w['properties'].update(qtText=label, qtTextAlignment='Left')
        w['geometry'].update(x=252, y=y, width=130, height=48)
        page['widgets'].append(w)
    for name, label, address, feedback, expected, warning in COMMANDS:
        w = copy.deepcopy(widgets['control-switch-mode'])
        w.update(widgetId='gen-control-' + name, title=label, visible=True, bindings=[], stateRules=[])
        x, y = rows[name]
        w['geometry'].update(x=x, y=y, width=210, height=48)
        w['action'] = dict(type='writeSetpoint', targetScreen='', nodeId='edge-1',
                           tagId=METER + '.gen_cmd_' + name, value='1',
                           requiresConfirmation=True, highPriority=False)
        w['properties'] = dict(qtText=label, qtTextColor='#303438', qtBackgroundColor='#F2FAFF',
                               qtFontSize=18, qtBorderColor='#2599D6', qtCornerRadius=6,
                               qtPressedColor='#D8EBFA', qtTheme='light',
                               controlWarning='HGM6110NC · RS-485 6 · 地址1：' + warning)
        if feedback:
            assert any(t['tagId'] == METER + '.' + feedback for t in tags)
            w['properties'].update(controlFeedbackTag=METER + '.' + feedback,
                                   controlFeedbackValue=str(expected))
        page['widgets'].append(w)
    ids = [w['widgetId'] for w in page['widgets']]
    assert len(ids) == len(set(ids))
    return config, tags, mappings, page


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--device-config', type=Path, required=True)
    parser.add_argument('--project', type=Path, required=True)
    parser.add_argument('--baseline-devices', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    new_indexes = {p['index'] for p in control_points()}
    for path in args.baseline_devices.glob('*.json'):
        for meter in load(path).get('meters', []):
            assert not new_indexes & {p['index'] for p in meter.get('points', [])}, path
    config, tags, mappings, page = bind_controls(load(args.device_config), load(args.project/'tags.json'),
        load(args.project/'runtime-map.json'), load(args.project/'screens/Control-Diesel.json'))
    args.output.mkdir()
    out = args.output/'project'
    shutil.copytree(args.project, out)
    save(args.output/'device.json', config)
    save(out/'tags.json', tags)
    save(out/'runtime-map.json', mappings)
    save(out/'screens/Control-Diesel.json', page)
    manifest = load(out/'manifest.json')
    manifest['packageVersion'] = '2.0.22-generator-controls-20261029'
    save(out/'manifest.json', manifest)
    checks = {p.relative_to(out).as_posix(): sha(p) for p in out.rglob('*') if p.is_file() and p.name != 'checksums.json'}
    save(out/'checksums.json', checks)
    changes = {n: dict(before=sha(args.project/n), after=sha(out/n)) for n in [*checks, 'checksums.json'] if sha(args.project/n) != sha(out/n)}
    assert set(changes) == {'tags.json', 'runtime-map.json', 'screens/Control-Diesel.json', 'manifest.json', 'checksums.json'}
    save(args.output/'changes.json', changes)
    save(args.output/'validation.json', dict(commands=7, electricalCommandsSent=0,
         noPulse=True, noRetry=True, wireValue='FF00 per FC05 example, not table 00FF typo',
         physicalControlVerified=False, independentBreakerFeedback=False))
    print(json.dumps(dict(commands=7, points=len(config['meters'][0]['points']), changedFiles=len(changes))))


if __name__ == '__main__':
    main()
