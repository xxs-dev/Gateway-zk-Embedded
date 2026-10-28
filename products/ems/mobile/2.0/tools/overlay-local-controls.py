"""Overlay local controls on a pulled, live SCADA project; never touch runtime/devices."""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import shutil
import zipfile

CODES = ['local_control_mode', 'local_manual_a_kw', 'local_manual_b_kw',
         'local_manual_c_kw', 'grid_reserve_status', 'local_control_applied',
         'local_final_active_a', 'local_final_active_b', 'local_final_active_c',
         'grid_reserve_phase_kw']
MODIFIED = {'manifest.json', 'tags.json', 'runtime-map.json', 'permissions.json', 'checksums.json',
            'screens/Control-Pcs.json', 'screens/Strategy-Overview.json'}


def read(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def write(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')


def hashes(root):
    result = {}
    for path in sorted(root.rglob('*')):
        if path.is_symlink():
            raise ValueError('Symlink in project: ' + str(path))
        if path.is_file():
            result[path.relative_to(root).as_posix()] = hashlib.sha256(path.read_bytes()).hexdigest()
    return result


def unique(items, field):
    result = {}
    for item in items:
        key = item[field]
        if key in result:
            raise ValueError(f'Duplicate {field}: {key}')
        result[key] = item
    return result


def navigation(widget_id, title, geometry, node_id):
    return dict(widgetId=widget_id, type='qtButton', title=title, geometry=geometry,
                zIndex=10, visible=True, bindings=[], stateRules=[],
                action=dict(type='navigate', targetScreen='LocalControl', nodeId=node_id,
                            tagId='', value='', requiresConfirmation=False, highPriority=False),
                properties=dict(qtText=title, qtFontSize=28, qtTextColor='#F3F8FA',
                                qtBackgroundColor='#14576B', qtTransparent=False))


def intersects(a, b):
    return (a['x'] < b['x'] + b['width'] and a['x'] + a['width'] > b['x'] and
            a['y'] < b['y'] + b['height'] and a['y'] + a['height'] > b['y'])


def overlay(source, controls, output):
    source, controls, output = (Path(p).resolve() for p in (source, controls, output))
    if output.exists() or output == source or source in output.parents or output in source.parents:
        raise ValueError('Output must be a new directory outside the source')
    if controls == output or output in controls.parents:
        raise ValueError('Output overlaps the control template')
    before = hashes(source)
    manifest = read(source / 'manifest.json')
    if manifest['entryScreen'] != 'Overview':
        raise ValueError('Expected the live Overview-entry project, not a generated XMind project')
    checksums = read(source / 'checksums.json')
    for name, expected in checksums.items():
        if before.get(name) != expected:
            raise ValueError('Source checksum mismatch: ' + name)
    pages = {p.stem: read(p) for p in (source / 'screens').glob('*.json')}
    if 'LocalControl' in pages:
        raise ValueError('Source already has LocalControl; review instead of reapplying')
    for name in ('Control-Pcs', 'Strategy-Overview'):
        if name not in pages or pages[name]['width'] != 1920 or pages[name]['height'] != 1080:
            raise ValueError('Expected live 1920x1080 page: ' + name)
    tags = read(source / 'tags.json')
    routes = read(source / 'runtime-map.json')
    tag_by_id, route_by_id = unique(tags, 'tagId'), unique(routes, 'tagId')
    route_by_index = unique(routes, 'index')
    node_ids = {r['nodeId'] for r in routes}
    if len(node_ids) != 1:
        raise ValueError('Multi-node overlay requires explicit review')
    node = next(iter(node_ids))
    donor_routes = unique(read(controls / 'runtime-map.json'), 'index')
    donor_tags = unique(read(controls / 'tags.json'), 'tagId')
    new_tags, new_routes, remap = [], [], {}
    for offset, code in enumerate(CODES):
        index = 740100 + offset
        if index in route_by_index:
            raise ValueError(f'Live control index already exists: {index}')
        route = copy.deepcopy(donor_routes[index])
        tag = copy.deepcopy(donor_tags[route['tagId']])
        if tag['pointCode'] != code or tag['indexFallback'] != index:
            raise ValueError(f'Control template contract mismatch: {index}')
        if route['sharedMemoryName'] != 'gateway_point_store_ems_virtual':
            raise ValueError(f'Unexpected control shared memory: {index}')
        writable = offset in (0, 1, 2, 3, 9)
        if bool(route['writable']) != writable or (tag['access'] != 'read') != writable:
            raise ValueError(f'Control access mismatch: {index}')
        if tag['tagId'] in tag_by_id or tag['tagId'] in route_by_id:
            raise ValueError('Live tag collision: ' + tag['tagId'])
        remap[route['tagId']] = route['tagId']
        route['nodeId'] = tag['nodeId'] = node
        new_tags.append(tag)
        new_routes.append(route)
    guard = dict(controlPauseGuard='true', pauseGuardModeTag=new_tags[0]['tagId'],
                 pauseGuardAppliedTag=new_tags[5]['tagId'])
    pcs = copy.deepcopy(pages['Control-Pcs'])
    indexed = unique(pcs['widgets'], 'widgetId')
    power = indexed.get('control-pcs-phase-power')
    stop = indexed.get('control-pcs-stop')
    if not power or power['type'] != 'pcsPhasePowerControl' or not stop:
        raise ValueError('Live PCS controls differ from reviewed layout')
    if {route_by_id[b['tagId']]['index'] for b in power['bindings']} != set(range(1318, 1324)):
        raise ValueError('Live phase-power bindings differ from reviewed controls')
    if route_by_id[stop['action']['tagId']]['index'] != 1202 or not stop['action']['requiresConfirmation']:
        raise ValueError('Live PCS stop action mismatch')
    stop.setdefault('properties', {}).update(guard)
    entry = navigation('local-controls-entry', '策略与三相手动功率',
                       dict(x=24, y=376, width=1872, height=126), node)
    pcs['widgets'] = [entry if w['widgetId'] == power['widgetId'] else w for w in pcs['widgets']]
    strategy = copy.deepcopy(pages['Strategy-Overview'])
    strategy_entry = navigation('local-strategy-entry', '策略启停',
                               dict(x=1570, y=120, width=318, height=62), node)
    for w in strategy['widgets']:
        if w.get('visible', True) and w['type'] != 'qtFrame' and intersects(w['geometry'], strategy_entry['geometry']):
            raise ValueError('Strategy entry overlaps live content: ' + w['widgetId'])
    strategy['widgets'].append(strategy_entry)
    local = read(controls / 'screens/LocalControl.json')
    local['widgets'] = [w for w in local['widgets'] if not w['widgetId'].startswith(('chrome-', 'nav-'))]
    chrome = [copy.deepcopy(w) for w in pages['Control-Pcs']['widgets']
              if w['widgetId'].startswith(('chrome-', 'nav-'))]
    local['widgets'] = chrome + local['widgets']
    for w in local['widgets']:
        for b in w.get('bindings', []):
            b['nodeId'] = node
        action = w.get('action') or {}
        if action:
            action['nodeId'] = node
        if action.get('type') in ('writeSetpoint', 'pulse', 'toggle'):
            if action.get('tagId') not in remap or not action.get('requiresConfirmation') or action.get('highPriority'):
                raise ValueError('Unexpected write action in LocalControl template')
        if w['type'] == 'qtInput':
            if action.get('tagId') not in {t['tagId'] for t in new_tags[1:4]}:
                raise ValueError('Unexpected manual input target')
            w['properties'].update(guard)
    permissions = read(source / 'permissions.json')
    if not permissions.get('localAccess', {}).get('users'):
        raise ValueError('Live project lacks local accounts; do not invent deployment credentials')
    prefixes = permissions['localAccess'].setdefault('protectedScreenPrefixes', [])
    for screen_id in ('Control-Pcs', 'Strategy-Overview'):
        if not any(screen_id.startswith(prefix) for prefix in prefixes):
            raise ValueError('Live writable page is unprotected: ' + screen_id)
    if not any('LocalControl'.startswith(prefix) for prefix in prefixes):
        prefixes.append('LocalControl')
    candidates = dict(pages, **{'Control-Pcs': pcs, 'Strategy-Overview': strategy, 'LocalControl': local})
    all_routes = unique(routes + new_routes, 'tagId')
    for screen_id, page in candidates.items():
        unique(page['widgets'], 'widgetId')
        for w in page['widgets']:
            action = w.get('action') or {}
            if action.get('type') == 'navigate' and action['targetScreen'] not in candidates:
                raise ValueError('Navigation references missing page: ' + action['targetScreen'])
            for b in w.get('bindings', []):
                if b['tagId'] not in all_routes:
                    raise ValueError('Unresolved binding: ' + b['tagId'])
            if w['type'] == 'pcsPhasePowerControl':
                raise ValueError('Unexpected additional direct PCS controller: ' + screen_id)
            targets = [action.get('tagId')] if action.get('type') in ('writeSetpoint', 'toggle', 'pulse') else []
            if w['type'] == 'qtInput':
                targets += [b['tagId'] for b in w.get('bindings', [])]
            if any(all_routes[t]['index'] in range(1318, 1324) for t in targets if t in all_routes):
                raise ValueError('Direct PCS power write remains: ' + w['widgetId'])
    output.mkdir(parents=True)
    project = output / 'scada-project'
    shutil.copytree(source, project)
    updated_manifest = dict(manifest, packageVersion='2.1.4-local-control')
    # Preserve every original byte except the explicitly requested version value.
    manifest_bytes = (source / 'manifest.json').read_bytes()
    old_version = json.dumps(manifest['packageVersion']).encode('utf-8')
    new_version = json.dumps(updated_manifest['packageVersion']).encode('utf-8')
    if manifest_bytes.count(old_version) != 1:
        raise ValueError('Manifest version token is not unique')
    (project / 'manifest.json').write_bytes(manifest_bytes.replace(old_version, new_version, 1))
    for name, value in [('tags.json', tags + new_tags), ('runtime-map.json', routes + new_routes),
                        ('permissions.json', permissions), ('screens/Control-Pcs.json', pcs),
                        ('screens/Strategy-Overview.json', strategy), ('screens/LocalControl.json', local)]:
        write(project / name, value)
    after = hashes(project)
    write(project / 'checksums.json', {k: v for k, v in after.items() if k != 'checksums.json'})
    after = hashes(project)
    changed = sorted(k for k in before if before[k] != after.get(k))
    if set(changed) - MODIFIED or hashes(source) != before:
        raise RuntimeError('Overlay preservation invariant failed')
    report = dict(source=str(source), sourceManifest=manifest, sourceHashes=before, outputHashes=after,
                  metadataDifferences={'packageVersion': {'before': manifest['packageVersion'],
                                                         'after': updated_manifest['packageVersion']}},
                  sourceScreenCount=len(pages), outputScreenCount=len(candidates), changedFiles=changed,
                  addedFiles=sorted(set(after) - set(before)),
                  preservedOriginalTags=len(tags), addedTags=len(new_tags),
                  permissionsChange='append LocalControl prefix only; credentials/roles/session retained',
                  runtimeTouched=False, modeInitialValueTouched=False,
                  requires='paired 740100-740109 runtime and Qt pause guard; never deploy UI alone')
    write(output / 'overlay-report.json', report)
    with zipfile.ZipFile(output / 'candidate.kyscada', 'w', zipfile.ZIP_DEFLATED) as archive:
        for path in sorted(project.rglob('*')):
            if path.is_file():
                archive.write(path, path.relative_to(project).as_posix())
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--controls', type=Path, required=True, help='Reviewed control-template project; only LocalControl and ten tags/routes imported')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = overlay(args.source, args.controls, args.output)
    print(json.dumps({k: result[k] for k in ('sourceScreenCount', 'outputScreenCount', 'changedFiles', 'addedFiles', 'addedTags')}, indent=2))
