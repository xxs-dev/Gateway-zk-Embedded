"""Read-only production HTTP inspection against the real private owner fixture."""
import copy
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile

from production_runtime_baseline_test import ProductionRuntimeTest


def main():
    probe = str(Path(sys.argv[1]).resolve(strict=True))
    fixture = ProductionRuntimeTest('test_production_entry_exists_without_changing_legacy_actions')
    fixture.setUp()
    results = []
    try:
        owner, previous, candidate = fixture.signed_config_owner_fixture()
        entry = fixture.real_owner_entry(owner)
        with owner.lease():
            entry.production_ota_owner(owner, owner.read())
        fixture.assertEqual(owner.read()['phase'], 'OWNER_FINISHED')
        fixture.assertEqual(fixture.g.production_context(fixture.home), candidate)
        config = {'ota': {'enabled': True, 'requireSignature': True, 'checksumRequired': True,
                         'stagingDir': str(fixture.home / 'ota/staging'),
                         'applyScript': str(fixture.home / 'bin/offline-runtime-upgrade.py'),
                         'signaturePublicKeyPath': str(fixture.path('/etc/modbus-gateway/ota.pub'))}}
        app = fixture.root / 'http-inspection-app.json'
        def tree():
            return {str(p.relative_to(fixture.home)): (p.lstat().st_mode,
                    hashlib.sha256(p.read_bytes()).hexdigest())
                    for p in fixture.home.rglob('*') if p.is_file() and not p.is_symlink()}
        def call(name='valid', expected=200, settings=None):
            app.write_text(json.dumps(settings or config))
            before = tree()
            with tempfile.TemporaryDirectory(prefix='inspection-auth-') as auth:
                process = subprocess.Popen([probe, str(fixture.home), str(app),
                    str(fixture.home / 'config/runtime/device_identity.json'), auth, name],
                    stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                try:
                    out, err = process.communicate(timeout=15)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.communicate()
                    raise
                fixture.assertEqual(process.returncode, 0, err.decode(errors='replace'))
            fixture.assertEqual(before, tree(), name + ': read changed runtime files')
            status, body = out.split(b'\r\n\r\n', 1)
            fixture.assertTrue(status.startswith(('HTTP/1.1 ' + str(expected)).encode()), name + ': ' + repr(out[:150]))
            value = json.loads(body)
            results.append({'case': name, 'httpStatus': expected, 'runtimeFilesUnchanged': True})
            return value
        first = call()
        fixture.assertEqual(first['format'], 'gateway-production-inspection/2')
        fixture.assertEqual(first['configSha256'], candidate['configSha256'])
        for relative in candidate['configSha256']:
            if relative.startswith('tls/'):
                content = (fixture.home / 'config/runtime' / relative).read_text().strip()
                if len(content) >= 8:
                    fixture.assertNotIn(content, json.dumps(first))
        legacy = call('legacy')
        fixture.assertEqual(legacy['format'], 'gateway-production-inspection/1')
        fixture.assertNotIn('configSha256', legacy)
        fixture.assertEqual(first['requestId'], 'INSPECT_1')
        fixture.assertEqual(first['machineCode'], previous['machineCode'])
        fixture.assertEqual(json.loads(first['baselineJson']), candidate)
        fixture.assertEqual(hashlib.sha256(first['baselineJson'].encode()).hexdigest(), first['baselineSha256'])
        fixture.assertFalse(first['runtimeVerified'])
        fixture.assertFalse(first['reservationCreated'])
        runtime = fixture.home / 'config/runtime'
        for name, relative in [('runtime-drift', 'apps/monitor-service.json'),
                               ('protected-drift', 'tls/mqtt-service-stunnel.conf')]:
            file = runtime / relative
            if not file.exists():
                # Select a real fixture pin of the corresponding category.
                paths = [p for p in candidate['configSha256'] if (p.startswith('tls/') if name == 'protected-drift' else p.startswith('apps/'))]
                fixture.assertTrue(paths, name)
                file = runtime / paths[0]
            original = file.read_bytes()
            try:
                file.write_bytes(original + b'\n')
                call(name, 409)
            finally:
                file.write_bytes(original)
        added = runtime / 'unexpected-config.json'
        try:
            added.write_text('{}\n')
            call('untracked-runtime-file', 409)
        finally:
            added.unlink()
        file = runtime / next(iter(candidate['configSha256']))
        backup = fixture.root / 'runtime-original-private'
        file.rename(backup)
        try:
            call('missing-runtime-file', 409)
            file.symlink_to(backup)
            call('symlink-runtime-file', 409)
            file.unlink()
            file.hardlink_to(backup)
            call('hardlink-runtime-file', 409)
            file.unlink()
        finally:
            if file.is_symlink() or file.exists(): file.unlink()
            backup.rename(file)
        original_mode = file.stat().st_mode & 0o777
        try:
            file.chmod(0o666)
            call('writable-runtime-file', 409)
        finally:
            file.chmod(original_mode)
        original = file.read_bytes()
        try:
            with file.open('wb') as handle: handle.truncate(4 * 1024 * 1024 + 1)
            call('oversize-runtime-file', 409)
        finally:
            file.write_bytes(original)
        link = runtime / 'external-directory'
        try:
            link.symlink_to(fixture.root, target_is_directory=True)
            call('symlink-runtime-directory', 409)
        finally:
            link.unlink()
        for name, status in [('wrong-password', 401), ('unprovisioned', 503),
                             ('sensitive-disabled', 403), ('wrong-target', 409), ('duplicate', 409), ('get', 404)]:
            call(name, status)
        for field, value in [('enabled', False), ('requireSignature', False), ('checksumRequired', False),
                             ('allowInsecureHttp', True), ('applyScript', str(fixture.home / 'bin/ota-apply.sh'))]:
            changed = copy.deepcopy(config); changed['ota'][field] = value
            call('policy-' + field, 409, changed)
        pointer = fixture.runtime.root / 'current.json'
        baseline = fixture.runtime.root / 'baselines' / (candidate['baselineId'] + '.json')
        health = fixture.runtime.root / 'transactions' / json.loads(pointer.read_bytes())['transaction'] / 'health.json'
        for name, file in [('baseline-drift', baseline), ('health-drift', health),
                           ('installer-drift', fixture.home / 'bin/offline-runtime-upgrade.py'),
                           ('key-drift', fixture.path('/etc/modbus-gateway/ota.pub'))]:
            original = file.read_bytes()
            try:
                file.write_bytes(original + b'\n')
                call(name, 409)
            finally:
                file.write_bytes(original)
        pending = fixture.runtime.root / 'pending.json'
        fixture.assertFalse(pending.exists())
        try:
            pending.write_text('{}\n')
            call('pending-transaction', 409)
        finally:
            pending.unlink()
        original = baseline.read_bytes()
        backup = baseline.with_suffix('.private-original')
        baseline.rename(backup)
        try:
            baseline.symlink_to(backup)
            call('symlink-baseline', 409)
        finally:
            baseline.unlink(); backup.rename(baseline)
        fixture.assertEqual(baseline.read_bytes(), original)
        final = call('restored')
        fixture.assertEqual(first['baselineSha256'], final['baselineSha256'])
        report = {'format': 'gateway-production-inspection-test/1', 'passed': True, 'cases': results,
                  'realDeviceVerified': False, 'scope': 'Actual HTTP authentication and read-only C++ inspection; original Python owner file transaction with substituted donor/systemd environment.'}
        print(json.dumps(report, sort_keys=True))
        if len(sys.argv) > 2:
            Path(sys.argv[2]).write_text(json.dumps(report, indent=2) + '\n')
    finally:
        fixture.doCleanups()


if __name__ == '__main__':
    main()
