"""Explicit local bridge: real signatures/file transactions, substituted service environment."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys

from production_runtime_baseline_test import ProductionRuntimeTest


def process_identity(pid):
    root = Path('/proc') / str(pid)
    return dict(pid=pid, start=root.joinpath('stat').read_text().rpartition(') ')[2].split()[19],
                group=os.getpgid(pid), session=os.getsid(pid),
                commandSha256=hashlib.sha256(root.joinpath('cmdline').read_bytes()).hexdigest())


def stop_owned(identity):
    try:
        if process_identity(identity['pid']) != identity or identity['group'] != identity['pid']:
            raise RuntimeError('owned bridge process identity changed')
        os.killpg(identity['group'], signal.SIGTERM)
        for _ in range(50):
            import time
            time.sleep(0.1)
            try:
                if process_identity(identity['pid']) != identity:
                    return
            except (FileNotFoundError, ProcessLookupError):
                return
        if process_identity(identity['pid']) == identity:
            os.killpg(identity['group'], signal.SIGKILL)
    except (FileNotFoundError, ProcessLookupError):
        pass


def main():
    if len(sys.argv) == 3 and sys.argv[1] == '--stop-runner':
        exchange = Path(sys.argv[2]).resolve(strict=True)
        for name in ('active-child.json', 'runner.json'):
            record = exchange / name
            if record.is_file():
                stop_owned(json.loads(record.read_bytes()))
        print('owned bridge stop complete', flush=True)
        return
    parser = argparse.ArgumentParser()
    parser.add_argument('--probe', required=True)
    parser.add_argument('--wire', required=True)
    parser.add_argument('--exchange', type=Path, required=True)
    parser.add_argument('--full', action='store_true')
    args = parser.parse_args()
    if os.geteuid() != 0:
        raise RuntimeError('root-owned private fixture required')
    exchange = args.exchange.resolve(strict=True)
    (exchange / 'runner.json').write_text(json.dumps(process_identity(os.getpid())) + '\n')
    def interrupted(signum, frame):
        raise InterruptedError('owned bridge runner terminated')
    signal.signal(signal.SIGTERM, interrupted)
    fixture = ProductionRuntimeTest('test_production_entry_exists_without_changing_legacy_actions')
    succeeded = False
    protocol_output = sys.stdout
    sys.stdout = sys.stderr
    try:
        fixture.setUp()
        platform_key = fixture.root / 'platform-public.pem'
        device_key = fixture.root / 'device-private.pem'
        for source, target in (('platform-public.pem', platform_key), ('device-private.pem', device_key)):
            shutil.copyfile(exchange / source, target)
            target.chmod(0o600)
        fixture.write(fixture.path('/etc/modbus-gateway/ota.pub'), platform_key.read_bytes(), 0o600)
        for baseline in (fixture.previous, fixture.candidate):
            baseline['trust']['publicKeySha256'] = fixture.m.digest(platform_key)

        def run(name, command, expected=0):
            process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                       start_new_session=True)
            identity = process_identity(process.pid)
            (exchange / 'active-child.json').write_text(json.dumps(identity) + '\n')
            timed_out = False
            try:
                out, err = process.communicate(timeout=45)
            except subprocess.TimeoutExpired:
                timed_out = True
                # This session/process group was created solely for this exact child.
                os.killpg(process.pid, signal.SIGTERM)
                try:
                    out, err = process.communicate(timeout=5)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    out, err = process.communicate(timeout=5)
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
            except BaseException:
                stop_owned(identity)
                out, err = process.communicate(timeout=5)
                (exchange / (name + '.stdout.raw')).write_bytes(out)
                (exchange / (name + '.stderr.raw')).write_bytes(err)
                (exchange / (name + '.exit.txt')).write_text(str(process.returncode) + '\n')
                raise
            (exchange / (name + '.stdout.raw')).write_bytes(out)
            (exchange / (name + '.stderr.raw')).write_bytes(err)
            (exchange / (name + '.exit.txt')).write_text(str(process.returncode) + '\n')
            if timed_out:
                (exchange / (name + '.timeout.json')).write_text(json.dumps(
                    dict(timeoutSeconds=45, processId=process.pid, exit=process.returncode)) + '\n')
                raise TimeoutError(name + ': owned native process group timed out and was reaped')
            if process.returncode != expected:
                raise AssertionError(name + ' failed: ' + err.decode(errors='replace'))
            return out, err

        def command(expected):
            if sys.stdin.readline().strip() != expected:
                raise RuntimeError('bridge command mismatch: ' + expected)

        def admit(artifact, version, package_type):
            import tarfile
            with tarfile.open(artifact, 'r:gz') as archive:
                manifest = json.load(archive.extractfile('runtime-manifest.json' if args.full else 'manifest.json'))
            target = fixture.g.production_context(fixture.home)['machineCode'] if args.full else manifest['machineCode']
            metadata = dict(version=version, packageType=package_type,
                            machineCode=target, artifactSha256=fixture.m.digest(artifact),
                            artifactSize=artifact.stat().st_size, manifest=manifest)
            if not args.full:
                metadata['candidateSha256'] = hashlib.sha256((json.dumps(
                    manifest['productionV2']['candidate'], sort_keys=True) + '\n').encode()).hexdigest()
            fixture.assertEqual(package_type, 'full' if args.full else 'config')
            shutil.copyfile(artifact, exchange / 'release.tar.gz')
            (exchange / 'ready.json').write_text(json.dumps(metadata, sort_keys=True) + '\n')
            print('READY', file=protocol_output, flush=True)
            command('INSTALL')
            # Only the donor/systemd boundary is substituted. Native execute still verifies
            # the installation signature and TAR checksum before generating intent/token.
            fixture.write(fixture.home / 'local-admission/offline-runtime-upgrade.py',
                          '# private donor boundary placeholder; never executed\n', 0o700)
            fixture.write(fixture.home / 'local-admission/ota-health-result.py',
                          'import sys\n'
                          'if sys.argv[1] not in ("--handoff", "--handoff-finish"): raise SystemExit(2)\n', 0o700)
            request = json.loads((exchange / 'install.json').read_bytes())
            fixture.assertEqual(request['machineCode'], metadata['machineCode'])
            fixture.assertEqual(request['version'], metadata['version'])
            fixture.assertEqual(request['sha256'], metadata['artifactSha256'])
            fixture.assertEqual(request['size'], metadata['artifactSize'])
            negative = dict(request, jobId='INVALID_SIGNATURE_LOCAL')
            negative['signature'] = ('A' if request['signature'][0] != 'A' else 'B') + request['signature'][1:]
            negative_path = fixture.root / 'negative-install.json'
            negative_path.write_text(json.dumps(negative))
            staging = fixture.home / 'ota/staging'
            before = fixture.m.tree_hashes(fixture.home / 'config/runtime')
            admission = '--admit-full-owner-local' if args.full else '--admit-owner-local'
            _, error = run('admission-negative', [args.probe, admission, str(negative_path),
                str(platform_key), str(artifact), str(staging)], expected=1)
            fixture.assertIn(b'native admission rejected stage=failed cause=ota signature verification failed', error)
            fixture.assertFalse((staging / 'ota_job_INVALID_SIGNATURE_LOCAL.json').exists())
            fixture.assertEqual(before, fixture.m.tree_hashes(fixture.home / 'config/runtime'))
            run('admission', [args.probe, admission, str(exchange / 'install.json'),
                str(platform_key), str(artifact), str(staging)])
            intent = json.loads((staging / ('ota_job_' + request['jobId'] + '.json')).read_bytes())
            fixture.assertEqual(intent['jobId'], request['jobId'])
            fixture.assertEqual(intent['artifactSha256'], fixture.m.digest(artifact))
            shutil.copyfile(staging / ('ota_job_' + request['jobId'] + '.json'), exchange / 'intent.json')
            return intent

        if args.full:
            # Existing full fixture creates a runtime-release-v1 TAR; no owner proof is invented.
            owner = fixture.owner_fixture(v2=True, real_entry=True, admit=admit)
            previous = fixture.g.production_context(fixture.home)
            candidate = None
        else:
            owner, previous, candidate = fixture.signed_config_owner_fixture(admit=admit)
        intent_before = owner.intent_path.read_bytes()
        entry = fixture.real_owner_entry(owner)
        with owner.lease():
            entry.production_ota_owner(owner, owner.read())
        fixture.assertEqual(owner.read()['phase'], 'OWNER_FINISHED')
        if args.full:
            candidate = fixture.g.production_context(fixture.home)
            fixture.assertEqual(candidate['release'], '1.1')
        else:
            fixture.assertEqual(fixture.g.production_context(fixture.home), candidate)
        fixture.assertEqual(owner.intent_path.read_bytes(), intent_before)
        run('reconcile', [args.probe, '--reconcile-existing', str(owner.root), owner.job])
        raw_proof, _ = run('read-proof', [args.probe, '--read-full-existing' if args.full else '--read-existing', str(owner.root),
            owner.job, previous['machineCode'], owner.token])
        proof = json.loads(raw_proof)
        health_path = owner.root / ('health_' + owner.job + '.txt')
        health = json.loads(health_path.read_bytes())
        intent = json.loads(intent_before)
        transaction = 'OTA_' + hashlib.sha256((owner.job + owner.token).encode()).hexdigest()[:32]
        state_dir = fixture.runtime.root / 'transactions' / transaction
        state = json.loads((state_dir / 'state.json').read_bytes())
        expected_proof = dict(outcome='HEALTHY_APPLIED', jobId=owner.job,
            machineCode=intent['machineCode'], occurrenceId=owner.token, version=intent['version'],
            artifactSha256=fixture.m.digest(fixture.root / 'release.tar.gz'), artifactSize=int(intent['artifactSize']),
            transaction=transaction, candidateSha256=state['candidateSha256'],
            intentSha256=fixture.m.digest(owner.intent_path), healthSha256=fixture.m.digest(health_path),
            healthTs=int(health['ts']))
        if args.full:
            parent_sha = fixture.m.digest(state_dir / 'previous.json')
            fixture.assertEqual(state['previousSha256'], parent_sha)
            fixture.assertEqual(candidate['parentSha256'], parent_sha)
            fixture.assertEqual(json.loads((state_dir / 'approval.json').read_bytes())['parentSha256'], parent_sha)
            expected_proof.update(packageType='full', parentSha256=parent_sha)
        fixture.assertEqual(proof, expected_proof)
        for name, path in {'health.json': health_path, 'handoff.json': owner.path,
                           'final.txt': owner.root / ('ota_final_' + owner.job + '.txt'),
                           'audit.json': owner.root / ('ota_reconcile_' + owner.job + '.json'),
                           **{'transaction-' + name: state_dir / name for name in
                              ('approval.json', 'candidate.json', 'previous.json', 'state.json', 'health.json')}}.items():
            shutil.copyfile(path, exchange / name)
        print('INSTALLED', file=protocol_output, flush=True)
        command('QUERY')
        def home_files():
            return {str(p.relative_to(fixture.home)): (p.stat().st_mode, fixture.m.digest(p))
                    for p in fixture.home.rglob('*') if p.is_file() and not p.is_symlink()}
        before = home_files()
        run('native-query', [args.wire, '--original-query-wire-bytes', str(exchange / 'challenge.json'),
            str(platform_key), str(device_key), str(owner.root), previous['machineCode'],
            str(exchange / 'response.json')])
        fixture.assertEqual(home_files(), before)
        response = json.loads((exchange / 'response.json').read_bytes())
        for key, value in proof.items():
            fixture.assertEqual(str(response[key]), str(value), key)
        print('QUERIED', file=protocol_output, flush=True)
        command('QUIT')
        succeeded = True
    finally:
        if not succeeded and hasattr(fixture, 'temp'):
            # Suppress only TemporaryDirectory removal; restore all fixture patches.
            fixture.temp._finalizer.detach()
            fixture._cleanups = [(fn, pos, kw) for fn, pos, kw in fixture._cleanups
                                 if getattr(fn, '__self__', None) is not fixture.temp]
            (exchange / 'failure-root.json').write_text(json.dumps(
                dict(privateRoot=str(fixture.root), retained=True)) + '\n')
        fixture.doCleanups()
        sys.stdout = protocol_output


if __name__ == '__main__':
    main()
