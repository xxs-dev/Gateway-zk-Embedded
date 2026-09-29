"""Gated diagnostics GET -> strict JSON -> offscreen capture -> gzip retrieval."""
import argparse
import hashlib
import http.client
import json
import math
import os
from pathlib import Path
import re
import shlex
import time
from urllib.parse import urlparse, urlencode


def sha(data):
    return hashlib.sha256(data).hexdigest()


def pointer(value, path):
    if path == '':
        return value
    if not path.startswith('/'):
        raise ValueError('JSON pointer must start with /')
    for part in path[1:].split('/'):
        part = part.replace('~1', '/').replace('~0', '~')
        value = value[int(part)] if isinstance(value, list) else value[part]
    return value


def normalize(raw, contract, sample_time):
    points = pointer(raw, contract['pointsPointer'])
    if not isinstance(points, list):
        raise ValueError('API points must be an array; pagination must be reviewed')
    result, seen, missing_expiry = [], set(), []
    for point in points:
        item = {key: pointer(point, contract['fields'][key])
                for key in ('index', 'value', 'quality', 'ts', 'stale')}
        try:
            item['expireAt'] = pointer(point, contract['fields']['expireAt'])
        except KeyError:
            item['expireAt'] = 0
            missing_expiry.append(item['index'])
        for key, low, high in [('index', 0, 2**32-1), ('quality', -2**31, 2**31-1),
                               ('ts', 0, 2**53-1), ('expireAt', 0, 2**53-1)]:
            if type(item[key]) is not int or not low <= item[key] <= high:
                raise ValueError('Invalid integer: ' + key)
        if type(item['value']) not in (int, float) or not math.isfinite(item['value']):
            raise ValueError('Invalid point value')
        if type(item['stale']) is not bool or item['index'] in seen:
            raise ValueError('Invalid stale or duplicate index')
        seen.add(item['index'])
        result.append(item)
    if type(sample_time) is not int or not 0 < sample_time < 2**53:
        raise ValueError('Invalid snapshot time')
    return {'sampleTimestampMs': sample_time, 'points': result,
            'missingExpireAtIndexes': missing_expiry, 'missingExpireAtMeaning': 'unknown, not unexpired'}


def validate(plan):
    if plan.get('userConfirmedDeployed') is not True or plan.get('keplerAccepted') is not True:
        raise ValueError('STOP: explicit deployed confirmation AND Kepler acceptance required')
    if not plan.get('keplerEvidence'):
        raise ValueError('Kepler acceptance evidence required')
    if not re.fullmatch(r'[a-f0-9]{64}', plan['helperSha256']):
        raise ValueError('Exact new JSON-only binary hash required')
    url = urlparse(plan['apiUrl'])
    if url.scheme != 'http' or url.hostname not in ('127.0.0.1', 'localhost') or url.username or url.password:
        raise ValueError('Only reviewed device-loopback HTTP diagnostics GET supported')
    if plan['expectedScreens'] != 18:
        raise ValueError('This acceptance requires all 18 pages')
    if not plan['projectDirectory'].startswith('/'):
        raise ValueError('Absolute deployed project required')
    if not plan['safetyConfigPath'].startswith('/') or not plan['safetyConfigPointer'].startswith('/'):
        raise ValueError('Reviewed on-device safety configuration location required')
    if plan['machineCode'] != 'COMM202600104' or not plan['expectedRelease'].startswith('/opt/modbus-gateway/scada/releases/'):
        raise ValueError('Exact COMM104 release identity required')


def meter_batches(routes, tags=None):
    by_tag = {t['tagId']: t for t in tags} if tags is not None else None
    if tags is not None and len(by_tag) != len(tags):
        raise ValueError('Duplicate project tag ID')
    groups, seen = {}, set()
    for route in routes:
        index = route['index']
        meter = by_tag[route['tagId']]['meterCode'] if by_tag is not None else route['meterCode']
        if type(index) is not int or not 0 <= index < 2**32 or index in seen or not meter:
            raise ValueError('Invalid/duplicate route index or empty meter')
        seen.add(index)
        groups.setdefault(meter, []).append(index)
    return [(meter, sorted(indexes)) for meter, indexes in sorted(groups.items())]


def safety_disabled(config, path, evidence=None, binary_hash=None):
    try:
        enabled = pointer(config, path)
    except KeyError:
        evidence = evidence or {}
        required = ('binaryPath', 'binarySha256', 'versionEvidence', 'modelDefaultEvidence', 'effectiveLoaderEvidence')
        if (evidence.get('effectivePointer') != path or evidence.get('defaultValue') is not False
                or not all(evidence.get(key) for key in required)
                or not re.fullmatch(r'[a-f0-9]{64}', evidence.get('binarySha256', ''))
                or binary_hash != evidence['binarySha256']):
            raise ValueError('STOP: absent safety flag needs version-matched effective-default evidence')
        return {'basis': 'version-verified-default-false', 'evidence': evidence}
    if enabled is not False:
        raise ValueError('STOP: scadaUpperComputerSafety enabled or unknown; never modify it')
    return {'basis': 'explicit-false', 'effectivePointer': path}


def execute(plan, helper, output):
    validate(plan)  # No network operation precedes this gate.
    helper = Path(helper)
    if sha(helper.read_bytes()) != plan['helperSha256']:
        raise ValueError('Helper hash mismatch')
    output = Path(output)
    output.mkdir(parents=True, exist_ok=False)
    import paramiko
    client = paramiko.SSHClient()
    client.load_system_host_keys()  # Reject unknown hosts, never AutoAddPolicy.
    password = os.environ.get('GRID_CAPTURE_PASSWORD')
    client.connect(plan['host'], username=plan['username'], password=password, timeout=10)
    remote = '/tmp/scada-json-capture-' + time.strftime('%Y%m%d-%H%M%S')
    quote = shlex.quote

    def run(command, allowed=(0,)):
        _, stdout, stderr = client.exec_command(command, timeout=120)
        data, error = stdout.read(), stderr.read()
        code = stdout.channel.recv_exit_status()
        if code not in allowed:
            raise RuntimeError('Remote command failed: ' + error.decode(errors='replace'))
        return data

    try:
        with client.open_sftp() as sftp:
            sftp.get_channel().settimeout(30)
            with sftp.open('/opt/modbus-gateway/config/runtime/device_identity.json', 'rb') as identity_file:
                identity = json.loads(identity_file.read())
            if identity['machineCode'] != plan['machineCode']:
                raise ValueError('Wrong device identity; stopping before sampling/capture')
            release = run('readlink -f ' + quote(plan['projectDirectory'])).decode().strip()
            if release != plan['expectedRelease']:
                raise ValueError('Unexpected deployed release')
            with sftp.open(plan['safetyConfigPath'], 'rb') as config:
                safety_bytes = config.read()
            if sha(safety_bytes) != plan['expectedSafetyConfigSha256']:
                raise ValueError('Monitor config differs from reviewed baseline')
            command_line = run('cat /proc/994/cmdline').replace(b'\x00', b' ').decode().strip()
            if command_line != '/opt/modbus-gateway/bin/SystemMonitor --app-config ' + plan['safetyConfigPath']:
                raise ValueError('Monitor process/config identity differs from reviewed evidence')
            evidence = plan.get('safetyDefaultEvidence')
            binary_hash = None
            if evidence:
                binary_hash = run('sha256sum ' + quote(evidence['binaryPath'])).decode().split()[0]
            safety_basis = safety_disabled(json.loads(safety_bytes), plan['safetyConfigPointer'], evidence, binary_hash)
            run('mkdir -m 700 ' + quote(remote))
            sftp.put(str(helper), remote + '/scada_capture_json')
            if run('sha256sum ' + quote(remote + '/scada_capture_json')).decode().split()[0] != plan['helperSha256']:
                raise ValueError('Uploaded helper hash mismatch')
            run('chmod 700 ' + quote(remote + '/scada_capture_json'))
            run('cp -a -- ' + quote(plan['projectDirectory'] + '/.') + ' ' + quote(remote + '/project'))
            sftp.get(remote + '/project/runtime-map.json', str(output / 'runtime-map.json'))
            sftp.get(remote + '/project/tags.json', str(output / 'tags.json'))
            routes = json.loads((output / 'runtime-map.json').read_bytes())
            tags = json.loads((output / 'tags.json').read_bytes())
            # Existing diagnostics supports meterCode, not indices. Never assume index filtering.
            started = int(run('date +%s%3N').strip())
            points, batches, missing_expiry, source_times = [], [], [], []
            for number, (meter, indexes) in enumerate(meter_batches(routes, tags)):
                separator = '&' if '?' in plan['apiUrl'] else '?'
                url = plan['apiUrl'] + separator + urlencode({'meterCode': meter})
                parsed = urlparse(url)
                channel = client.get_transport().open_channel(
                    'direct-tcpip', (parsed.hostname, parsed.port or 80), ('127.0.0.1', 0), timeout=20)
                channel.settimeout(20)
                try:
                    # HTTPConnection closes a Connection: close socket after headers;
                    # unlike socket.makefile, ChannelFile cannot outlive Channel.close.
                    request = ('GET ' + parsed.path + '?' + parsed.query + ' HTTP/1.1\r\nHost: '
                               + parsed.netloc + '\r\nConnection: close\r\n\r\n')
                    channel.sendall(request.encode('ascii'))
                    response_http = http.client.HTTPResponse(channel)
                    response_http.begin()
                    if response_http.status != 200:
                        raise ValueError('Diagnostics GET returned HTTP ' + str(response_http.status))
                    chunks, size = [], 0
                    deadline = time.monotonic() + 30
                    while True:
                        if time.monotonic() > deadline:
                            raise TimeoutError('Diagnostics body deadline exceeded')
                        chunk = response_http.read(65536)
                        if not chunk:
                            break
                        chunks.append(chunk)
                        size += len(chunk)
                        if size > 8 * 1024 * 1024:
                            raise ValueError('Diagnostics response exceeds bounded sample size')
                    raw = b''.join(chunks)
                    if len(raw) > 8 * 1024 * 1024:
                        raise ValueError('Diagnostics response exceeds bounded sample size')
                finally:
                    channel.close()
                (output / f'api-response-{number:03}.json').write_bytes(raw)
                batch_time = int(run('date +%s%3N').strip())
                response = json.loads(raw)
                raw_points = pointer(response, plan['contract']['pointsPointer'])
                if len(raw_points) >= 2000:
                    raise ValueError('Meter response reached API cap; completeness cannot be claimed')
                if any(p['meterCode'] != meter for p in raw_points):
                    raise ValueError('API ignored meterCode filter')
                if any(p['machineCode'] != plan['machineCode'] for p in raw_points):
                    raise ValueError('API returned another station identity')
                source_time = response['ts']
                normalized = normalize(response, plan['contract'], source_time)
                source_times.append(source_time)
                missing_expiry.extend(i for i in normalized['missingExpireAtIndexes'] if i in set(indexes))
                batch = normalized['points']
                batch = [p for p in batch if p['index'] in set(indexes)]
                returned = {p['index'] for p in batch}
                points.extend(batch)
                batches.append(dict(meterCode=meter, requested=indexes, returned=sorted(returned),
                                    missing=sorted(set(indexes) - returned),
                                    responseSha256=sha(raw), receivedMs=batch_time, sourceTimestampMs=source_time))
            ended = int(run('date +%s%3N').strip())
            samples = {'sampleTimestampMs': max(source_times), 'points': points,
                       'missingExpireAtIndexes': missing_expiry, 'missingExpireAtMeaning': 'unknown, not unexpired'}
            sample_bytes = (json.dumps(samples, allow_nan=False) + '\n').encode()
            (output / 'samples.json').write_bytes(sample_bytes)
            sftp.put(str(output / 'samples.json'), remote + '/samples.json')
            log = run('timeout 90 ' + quote(remote + '/scada_capture_json') + ' --project ' + quote(remote + '/project')
                      + ' --samples ' + quote(remote + '/samples.json') + ' --output ' + quote(remote + '/pages'), (0, 2))
            (output / 'capture.log').write_bytes(log)
            sftp.get(remote + '/pages/capture-report.json', str(output / 'capture-report.json'))
            report = json.loads((output / 'capture-report.json').read_bytes())
            if (report['captureKind'] != 'offscreen-json-sample' or report['liveDataAccess'] is not False
                    or report['writeAttempts'] != 0 or len(report['pages']) != 18
                    or report['sampleInputSha256'] != sha(sample_bytes)):
                raise ValueError('Capture provenance failed')
            for page in report['pages']:
                if page['width'] != 1920 or page['height'] != 1080:
                    raise ValueError('Capture dimensions failed')
            run('tar -czf ' + quote(remote + '/pages.tar.gz') + ' -C ' + quote(remote) + ' pages')
            sftp.get(remote + '/pages.tar.gz', str(output / 'pages.tar.gz'))
            archive_hash = sha((output / 'pages.tar.gz').read_bytes())
            if run('sha256sum ' + quote(remote + '/pages.tar.gz')).decode().split()[0] != archive_hash:
                raise ValueError('Downloaded archive hash mismatch')
            (output / 'pipeline-report.json').write_text(json.dumps(dict(
                remoteDirectory=remote, machineCode=plan['machineCode'], deployedRelease=release,
                helperSha256=plan['helperSha256'], archiveSha256=archive_hash,
                batches=batches, sampleStartedMs=started, sampleEndedMs=ended,
                sourceTimestampMinMs=min(source_times), sourceTimestampMaxMs=max(source_times),
                atomicSnapshot=False, missingSamples=report['missingSampleIndexes'],
                safetyConfigSha256=sha(safety_bytes), scadaUpperComputerSafetyEnabled=False,
                safetyDecision=safety_basis, missingExpireAtIndexes=missing_expiry,
                missingExpireAtMeaning='unknown, not unexpired; original quality/stale retained',
                diagnosticGetMayPerformInternalMaintenance=True,
                keplerEvidence=plan['keplerEvidence']), indent=2), encoding='utf-8')
    finally:
        client.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--plan', required=True, type=Path)
    parser.add_argument('--helper', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--execute', action='store_true')
    args = parser.parse_args()
    plan = json.loads(args.plan.read_text(encoding='utf-8-sig'))
    if not args.execute:
        print('PREPARATION ONLY: no network; --execute and both explicit approvals required')
    else:
        execute(plan, args.helper, args.output)
