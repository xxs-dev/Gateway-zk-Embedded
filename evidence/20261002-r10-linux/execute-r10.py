"""L2 build-only review runner. Default local-only; board has no execution route."""
import argparse
import ast
import hashlib
import importlib.util
import json
from pathlib import Path
import stat

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('r10_local_inputs', HERE / 'prepare-r10.py')
inputs = importlib.util.module_from_spec(spec); spec.loader.exec_module(inputs)


def upload(sftp, directory, name, data, mode):
    with sftp.open(directory + '/' + name, 'wx') as output: output.write(data)
    sftp.chmod(directory + '/' + name, mode)


def check_sftp_parent(info, expected):
    # SFTPv3 has no dev/inode fields. Remote os.stat supplies those identities,
    # and build-child rechecks them before unpacking or launching a compiler.
    assert stat.S_ISDIR(info.st_mode)
    assert (info.st_uid, info.st_gid, stat.S_IMODE(info.st_mode)) == (
        expected['uid'], expected['gid'], expected['mode'])


def remote_code(manifest, expected):
    root = manifest['buildRoot']
    return ('from pathlib import Path\nimport json,hashlib,traceback\n'
            'try:\n'
            ' root=Path(' + repr(root) + ')\n'
            ' code=(root/"build-child.py").read_bytes()\n'
            ' assert hashlib.sha256(code).hexdigest()==' + repr(manifest['supplementalFiles']['build-child.py']['sha256']) + '\n'
            ' exec(compile(code,"build-child.py","exec"))\n'
            ' result=build(str(root),' + repr(manifest) + ',' + repr(expected) + ')\n'
            'except BaseException as error:\n'
            ' result=dict(phase="FAILED",primaryError=dict(type=type(error).__name__,message=str(error),traceback=traceback.format_exc(limit=12)))\n'
            'print(json.dumps(result))\n'
            'raise SystemExit(0 if result.get("phase")=="BUILT" and result.get("sharedUnchanged") and not any(result.get(k) for k in ("primaryError","postcheckErrors","receiptErrors")) else 1)\n')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--stage', choices=('build',), default='build')
    parser.add_argument('--execute-approved', action='store_true')
    parser.add_argument('--manifest-sha256')
    args = parser.parse_args()
    frozen = (HERE / 'input-manifest-L2.json').read_bytes(); manifest = json.loads(frozen)
    archive, computed = inputs.prepare()
    assert computed == manifest, 'fixed L2 input drift'
    digest = hashlib.sha256(frozen).hexdigest()
    if args.manifest_sha256 is not None: assert args.manifest_sha256 == digest
    base = inputs.primitives()
    plan = dict(stage='build-L2', manifestSha256=digest, sourceCommit=inputs.COMMIT,
                productCommit=inputs.PRODUCT_COMMIT, sourceArchiveBytes=len(archive),
                sourceArchiveSha256=manifest['sourceArchiveSha256'], sourceFiles=len(manifest['sourceFiles']),
                buildRoot=manifest['buildRoot'], targets=manifest['targets'], boardExecutionReady=False,
                executionAuthorized=False, connects=args.execute_approved, remoteResultAvailable=False)
    environment_code = (HERE / 'build-environment.py').read_bytes().decode()
    preflight = (environment_code + '\nimport json,traceback\ntry:\n result=inspect_environment()\n'
                 'except BaseException as error:\n'
                 ' print(json.dumps(dict(primaryError=dict(type=type(error).__name__,message=str(error),traceback=traceback.format_exc(limit=12)))))\n raise SystemExit(1)\n'
                 'print(json.dumps(result))\n')
    ast.parse(preflight, feature_version=(3, 6))
    ast.parse(remote_code(manifest, {}), feature_version=(3, 6))
    if not args.execute_approved:
        print(json.dumps(plan, indent=2)); return 0  # No authentication import or connection.
    assert args.manifest_sha256 is not None
    receipt_path = HERE / 'build-L2-result.json'
    with receipt_path.open('x') as output: output.write('{"phase":"LOCAL_RESERVED","sshStarted":false}\n')
    result = dict(plan, sshStarted=False); client = None
    try:
        helper = inputs.COORD / 'run_bounded_probe.py'
        connect_spec = importlib.util.spec_from_file_location('r10_existing_connection', helper)
        connection = importlib.util.module_from_spec(connect_spec); connect_spec.loader.exec_module(connection)
        root = manifest['buildRoot']
        files = {'source.tar': (archive, 0o400)}
        for name in ('build-child.py', 'build-group.py', 'build-environment.py', 'ownership-probe.cpp'):
            files[name] = ((HERE / name).read_bytes(), 0o500)
        client = connection.connect(manifest['buildHost']); result['sshStarted'] = True
        status, raw, error = base['invoke'](client, preflight, 60)
        result.update(preflightExitCode=status, preflightStdout=raw.decode(), preflightStderr=error.decode())
        assert status == 0 and not error, 'environment failed BEFORE upload'
        expected = json.loads(raw)
        with client.open_sftp() as sftp:
            sftp.get_channel().settimeout(30)
            parent = str(Path(root).parent).replace('\\', '/')
            info = sftp.lstat(parent)
            check_sftp_parent(info, expected['parent'])
            try: sftp.lstat(root)
            except FileNotFoundError: pass
            else: raise ValueError('fresh L2 root occupied; no replay')
            sftp.mkdir(root, mode=0o700)
            for name, (data, mode) in files.items(): upload(sftp, root, name, data, mode)
        code = remote_code(manifest, expected)
        ast.parse(code, feature_version=(3, 6))
        status, raw, error = base['invoke'](client, code, 1650)
        result.update(remoteExitCode=status, stdout=raw.decode('utf-8', 'replace'), stderr=error.decode('utf-8', 'replace'))
        if raw:
            result['execution'] = json.loads(raw); result['remoteResultAvailable'] = True
        with client.open_sftp() as sftp:
            sftp.get_channel().settimeout(30)
            names = ['build-result.json', 'build.log', 'ownership-build.log']
            names += [row['name'] + '-elf.log' for row in result.get('execution', {}).get('artifacts', [])]
            for name in names:
                try: info = sftp.lstat(root + '/' + name)
                except FileNotFoundError: continue
                assert stat.S_ISREG(info.st_mode) and info.st_size <= 16 * 1024**2
                with sftp.open(root + '/' + name, 'rb') as stream: data = stream.read(info.st_size + 1)
                assert len(data) == info.st_size
                with (HERE / ('L2-remote-' + name)).open('xb') as output: output.write(data)
            if status == 0 and not error:
                (HERE / 'L2-artifacts').mkdir()
                for row in result['execution']['artifacts']:
                    assert row['name'] in inputs.TARGETS + ['ownership-probe'] and row['bytes'] <= 32 * 1024**2
                    with sftp.open(root + '/build/' + row['name'], 'rb') as stream: data = stream.read(row['bytes'] + 1)
                    assert len(data) == row['bytes'] and inputs.sha(data) == row['sha256']
                    with (HERE / 'L2-artifacts' / row['name']).open('xb') as output: output.write(data)
                result['returnedArtifactHashesVerified'] = True
    except BaseException as error:
        result.update(errorType=type(error).__name__, errorMessage=str(error))
    finally:
        try:
            if client is not None: client.close()
            result['sshClosed'] = True
        except BaseException as error:
            result.update(sshClosed=False, closureError=str(error))
        try: receipt_path.write_text(json.dumps(result, indent=2) + '\n')
        except BaseException as error: result['localReceiptError'] = str(error)
    print(json.dumps({k: v for k, v in result.items() if k not in ('execution', 'stdout')}))
    return 0 if result.get('remoteExitCode') == 0 and result.get('returnedArtifactHashesVerified') and result.get('sshClosed') and not result.get('stderr') and not any(k in result for k in ('errorType','closureError','localReceiptError')) else 1


if __name__ == '__main__':
    raise SystemExit(main())
