"""Review runner. Default verifies frozen inputs only; explicit later scope required."""
import argparse
import ast
import hashlib
import importlib.util
import json
from pathlib import Path
import stat
import struct
import sys

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('r10_local_inputs', HERE / 'prepare-r10.py')
inputs = importlib.util.module_from_spec(spec); spec.loader.exec_module(inputs)


def upload(sftp, directory, name, data, mode):
    path = directory + '/' + name
    with sftp.open(path, 'wx') as output: output.write(data)
    sftp.chmod(path, mode)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--stage', choices=('build', 'board'), default='build')
    parser.add_argument('--execute-approved', action='store_true')
    parser.add_argument('--manifest-sha256')
    args = parser.parse_args()
    frozen = (HERE / 'input-manifest.json').read_bytes(); manifest = json.loads(frozen)
    archive, computed = inputs.prepare()
    assert computed == manifest, 'fixed input drift'
    digest = hashlib.sha256(frozen).hexdigest()
    if args.manifest_sha256 is not None: assert args.manifest_sha256 == digest
    base = inputs.primitives()
    plan = dict(stage=args.stage, manifestSha256=digest, sourceCommit=inputs.COMMIT,
                productCommit=inputs.PRODUCT_COMMIT, sourceArchiveBytes=len(archive),
                sourceArchiveSha256=manifest['sourceArchiveSha256'], sourceFiles=len(manifest['sourceFiles']),
                buildRoot=manifest['buildRoot'], boardRoot=manifest['boardRoot'], targets=manifest['targets'],
                executionAuthorized=False, connects=args.execute_approved, remoteResultAvailable=False)
    if not args.execute_approved:
        print(json.dumps(plan, indent=2)); return 0  # No auth import, SSH, WSL, build or fixture.
    assert args.manifest_sha256 is not None, 'approved exact manifest identity required'
    if args.stage == 'board': assert manifest['boardExecutionReady'], 'board draft is NOT executable scope'
    receipt_path = HERE / (args.stage + '-result.json')
    with receipt_path.open('x') as output: output.write('{"phase":"LOCAL_RESERVED","sshStarted":false}\n')
    result = dict(plan, sshStarted=False); client = None
    try:
        helper = inputs.COORD / 'run_bounded_probe.py'
        connect_spec = importlib.util.spec_from_file_location('r10_existing_connection', helper)
        connection = importlib.util.module_from_spec(connect_spec); connect_spec.loader.exec_module(connection)
        host = manifest['buildHost'] if args.stage == 'build' else manifest['boardHost']
        root = manifest['buildRoot'] if args.stage == 'build' else manifest['boardRoot']
        files = {}
        if args.stage == 'build':
            files['source.tar'] = (archive, 0o400)
            for name in ('build-child.py', 'build-group.py', 'ownership-probe.cpp'):
                files[name] = ((HERE / name).read_bytes(), 0o500)
            code = ('from pathlib import Path\nimport json,hashlib\n'
                    'root=Path(' + repr(root) + ')\n'
                    'code=(root/"build-child.py").read_bytes()\n'
                    'assert hashlib.sha256(code).hexdigest()==' + repr(manifest['supplementalFiles']['build-child.py']['sha256']) + '\n'
                    'exec(compile(code,"build-child.py","exec"))\n'
                    'result=build(str(root),' + repr(manifest) + ')\n'
                    'print(json.dumps(result))\n'
                    'raise SystemExit(0 if result["phase"]=="BUILT" and result["sharedUnchanged"] else 1)\n')
            seconds = 1500
        else:
            build_result = json.loads((HERE / 'build-result.json').read_bytes())
            built = build_result['execution']
            assert build_result['remoteExitCode'] == 0 and built['phase'] == 'BUILT' and built['sharedUnchanged']
            assert built['sourceCommit'] == inputs.COMMIT
            files['source/namespace-child.py'] = ((HERE / 'namespace-child.py').read_bytes(), 0o500)
            manifest['boardFiles'] = {}
            for artifact in built['artifacts']:
                name = artifact['name']; assert name in inputs.TARGETS + ['ownership-probe']
                data = (HERE / 'artifacts' / name).read_bytes()
                assert len(data) == artifact['bytes'] and inputs.sha(data) == artifact['sha256']
                assert data[:6] == b'\x7fELF\x02\x01' and struct.unpack_from('<H', data, 18)[0] == 183
                files['bin/' + name] = (data, 0o500)
            assert len(files) == 5
            for path, (data, mode) in files.items():
                directory, name = path.split('/')
                manifest['boardFiles'][name] = dict(directory=directory, bytes=len(data), sha256=inputs.sha(data), mode=mode)
            # Reuse exact frozen baseline code with its original input/probe pins.
            probe = (inputs.COORD / 'd2_device_readonly_probe.py').read_bytes().decode()
            body = (HERE / 'board-child.py').read_text()
            code = ('PROBE=' + repr(probe) + '\nNAMES=()\n' + base['BASELINE_CODE'] + '\n' + body + '\n'
                    'result=run(' + repr(root) + ',' + repr(manifest) + ',baseline,EXPECTED)\n'
                    'print(json.dumps(result))\n'
                    'raise SystemExit(0 if result.get("fixtureExitCode")==0 and result.get("baselineChanges")==[] and result.get("workRemoved") and result.get("remainingNamespaceProcesses")==[] and not any(k in result for k in ("errorType","cleanupError","postcheckError")) else 1)\n')
            seconds = 420
        ast.parse(code, feature_version=(3, 8))
        client = connection.connect(host); result['sshStarted'] = True
        if args.stage == 'build':
            preflight = ('import json,platform,os\nfrom pathlib import Path\n'
                         'assert platform.machine()=="x86_64"\n'
                         'paths=["/srv/build","/srv/build/Gateway-zk","/home/tronlong/Linux/SZR/aarch64/gcc-linaro-6.3.1-2017.05-x86_64_aarch64-linux-gnu/bin"]\n'
                         'assert all(Path(p).is_dir() for p in paths)\n'
                         'lines=[line.split() for line in Path("/proc/meminfo").read_text().splitlines() if line.startswith("MemAvailable:")]\n'
                         'assert len(lines)==1 and len(lines[0])==3 and lines[0][2]=="kB" and lines[0][1].isdigit()\n'
                         'available=int(lines[0][1])*1024\n'
                         'free=os.statvfs("/srv/build").f_bavail*os.statvfs("/srv/build").f_frsize\n'
                         'assert available>=5*1024**3 and free>=2*1024**3\n'
                         'print(json.dumps(dict(architecture=platform.machine(),bootId=Path("/proc/sys/kernel/random/boot_id").read_text().strip(),paths=paths,memAvailableBytes=available,freeBytes=free)))\n')
        else:
            preflight = 'PROBE=' + repr(probe) + '\nNAMES=()\n' + base['BASELINE_CODE'] + '\nprint(json.dumps(baseline()))\n'
        pre_status, pre_raw, pre_err = base['invoke'](client, preflight, 60)
        result.update(preflightExitCode=pre_status, preflightStdout=pre_raw.decode(), preflightStderr=pre_err.decode())
        assert pre_status == 0 and not pre_err
        expected = json.loads(pre_raw)
        if args.stage == 'board': code = 'EXPECTED=' + repr(expected) + '\n' + code
        with client.open_sftp() as sftp:
            parent = str(Path(root).parent).replace('\\', '/')
            assert stat.S_ISDIR(sftp.lstat(parent).st_mode)
            try: sftp.lstat(root)
            except FileNotFoundError: pass
            else: raise ValueError('fresh namespace occupied; no replay')
            sftp.mkdir(root, mode=0o700)
            if args.stage == 'board':
                sftp.mkdir(root + '/source', mode=0o700); sftp.mkdir(root + '/bin', mode=0o700)
            for name, (data, mode) in files.items(): upload(sftp, root, name, data, mode)
        status, raw, err = base['invoke'](client, code, seconds)
        result.update(remoteExitCode=status, stdout=raw.decode('utf-8', 'replace'), stderr=err.decode('utf-8', 'replace'))
        if raw: result['execution'] = json.loads(raw)
        if args.stage == 'build':
            with client.open_sftp() as sftp:
                names = ['build-result.json', 'build.log', 'ownership-build.log']
                names += [row['name'] + '-elf.log' for row in result.get('execution', {}).get('artifacts', [])]
                for name in names:
                    try: info = sftp.lstat(root + '/' + name)
                    except FileNotFoundError: continue
                    assert stat.S_ISREG(info.st_mode) and info.st_size <= 16 * 1024**2
                    with sftp.open(root + '/' + name, 'rb') as stream: data = stream.read(info.st_size + 1)
                    assert len(data) == info.st_size
                    with (HERE / ('remote-' + name)).open('xb') as output: output.write(data)
                if status == 0 and not err:
                    (HERE / 'artifacts').mkdir()
                    for row in result['execution']['artifacts']:
                        with sftp.open(root + '/build/' + row['name'], 'rb') as stream: data = stream.read(row['bytes'] + 1)
                        assert len(data) == row['bytes'] and inputs.sha(data) == row['sha256']
                        with (HERE / 'artifacts' / row['name']).open('xb') as output: output.write(data)
                    result['returnedArtifactHashesVerified'] = True
    except BaseException as error:
        result.update(errorType=type(error).__name__, errorMessage=str(error))
    finally:
        if client is not None: client.close()
        result['sshClosed'] = True
        receipt_path.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({k: v for k, v in result.items() if k not in ('execution', 'stdout')}))
    return 0 if result.get('remoteExitCode') == 0 and not result.get('stderr') and 'errorType' not in result else 1


if __name__ == '__main__':
    raise SystemExit(main())
