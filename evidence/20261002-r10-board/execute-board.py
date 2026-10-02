"""23-only proposal. Default pin verification has no authentication or network."""
import argparse
import ast
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import stat
import subprocess
import zipfile

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
BUILD_COMMIT = '9f581ac4155fa7ad27f18313fb846af5f9058fc4'
SOURCE_COMMIT = '438507cdb191b5b91c77eca2e672b4e74e06bd1f'
BASE_PATH = 'evidence/20261002-nativeboot-B2/execute-native-creator.py'
BASE_SHA = 'fee48c39ca46a3fc40cefed0782232183f582c397061ebac33979a117e52770a'
ZIP_PATH = 'evidence/20261002-r10-linux/L2-execution/raw-evidence.zip'
ZIP_SHA = 'fae73113e5fc1cd061d90fdbbe7ecf2b942b04e5a36696bca5cc5002413f69bf'
COORD = Path('D:/workspace/GatewaySuite-workspaces/realese1.0/coordinator/docs/audits/evidence/20261001-device-acceptance')
CONNECT_SHA = 'd8dfd569062fee7bbb022c6f4f49b93bdd4a0415a819d1c2e5bfcb16f2dc7ccf'
PROBE_SHA = '509a74fcf5658156773d1273b4e2222d5b8d687da57bc8cdaf0086a88765a221'
ROOT = '/tmp/gw-r10-68de70f-23-B1'
HOST = '192.168.22.23'
SCRIPTS = ('environment.py', 'board-group.py', 'namespace-child.py', 'board-run.py')


def sha(data): return hashlib.sha256(data).hexdigest()


def blob(commit, path):
    return subprocess.check_output(['git', 'cat-file', 'blob', commit+':'+path], cwd=str(REPO))


def elf_requirements(data):
    from elftools.elf.elffile import ELFFile
    elf = ELFFile(io.BytesIO(data))
    assert elf.elfclass == 64 and elf.little_endian and elf['e_machine'] == 'EM_AARCH64'
    interpreters = [s.get_interp_name() for s in elf.iter_segments() if s['p_type'] == 'PT_INTERP']
    assert interpreters == ['/lib/ld-linux-aarch64.so.1']
    needed = []
    for tag in elf.get_section_by_name('.dynamic').iter_tags():
        assert tag.entry.d_tag not in ('DT_RPATH', 'DT_RUNPATH', 'DT_FILTER', 'DT_AUXILIARY')
        if tag.entry.d_tag == 'DT_NEEDED': needed.append(tag.needed)
    indices = {}
    versions = elf.get_section_by_name('.gnu.version_r')
    if versions:
        for _, auxiliaries in versions.iter_versions():
            for auxiliary in auxiliaries: indices[auxiliary['vna_other'] & 0x7fff] = auxiliary.name
    table = elf.get_section_by_name('.dynsym'); versym = elf.get_section_by_name('.gnu.version')
    symbols = set()
    for i, symbol in enumerate(table.iter_symbols()):
        if symbol['st_shndx'] != 'SHN_UNDEF' or symbol['st_info']['bind'] != 'STB_GLOBAL': continue
        name = symbol.name; assert name
        ndx = versym.get_symbol(i)['ndx'] if versym else 'VER_NDX_GLOBAL'
        if isinstance(ndx, int) and (ndx & 0x7fff) > 1: name += '@' + indices[ndx & 0x7fff]
        symbols.add(name)
    assert needed and symbols
    return dict(needed=sorted(needed), symbols=sorted(symbols), interpreter=interpreters[0])


def prepare():
    archive = blob(BUILD_COMMIT, ZIP_PATH); assert sha(archive) == ZIP_SHA
    summary = json.loads(blob(BUILD_COMMIT, 'evidence/20261002-r10-linux/L2-execution/summary.json'))
    names = ('diagnostic_process_test', 'system_monitor_diagnostic_test', 'system_monitor_service_test', 'ownership-probe')
    files = {}; requirements = {}; pins = {}
    with zipfile.ZipFile(io.BytesIO(archive)) as z:
        for name in names:
            data = z.read('L2-artifacts/'+name)
            row = next(r for r in summary['files'] if r['path'] == 'L2-artifacts/'+name)
            assert len(data) == row['bytes'] and sha(data) == row['sha256']
            requirements[name] = elf_requirements(data); files[name] = data
            pins[name] = dict(directory='bin', bytes=len(data), sha256=sha(data), mode=0o500)
    for name in SCRIPTS:
        data = (HERE/name).read_bytes(); ast.parse(data.decode(), feature_version=(3, 6))
        files[name] = data; pins[name] = dict(directory='source', bytes=len(data), sha256=sha(data), mode=0o500)
    local = {}
    for name in ('execute-board.py', 'board_runner_test.py'):
        data = (HERE/name).read_bytes(); ast.parse(data.decode(), feature_version=(3, 6))
        local[name] = dict(bytes=len(data), sha256=sha(data))
    baseline = blob(SOURCE_COMMIT, BASE_PATH); assert sha(baseline) == BASE_SHA
    for name, expected in (('run_bounded_probe.py', CONNECT_SHA), ('d2_device_readonly_probe.py', PROBE_SHA)):
        assert sha((COORD/name).read_bytes()) == expected
    base = {'__name__': 'r10_board_baseline', '__file__': str(REPO/BASE_PATH)}
    exec(compile(baseline, BASE_PATH, 'exec'), base)
    manifest = dict(host=HOST, root=ROOT, productCommit='68de70fbc37cf3267116624a259a81510b2d74d3',
                    sourceCommit=SOURCE_COMMIT, buildEvidenceCommit=BUILD_COMMIT, buildArchiveSha256=ZIP_SHA,
                    remoteFiles=pins, localFiles=local, elfRequirements=requirements, baselineSha256=BASE_SHA,
                    connectSha256=CONNECT_SHA, probeSha256=PROBE_SHA, executionAuthorized=False,
                    cases=['diagnostic_process_test', 'system_monitor_diagnostic_test', 'system_monitor_service_test',
                           'ownership-probe fd', 'ownership-probe parent-death', 'ownership-probe supervisor-death'])
    return files, manifest, base


def baseline_code(base):
    # Reuse exact B2 reader/config/state proof; add bounded parent SHM inventory.
    return ('PROBE='+repr((COORD/'d2_device_readonly_probe.py').read_text())+'\nNAMES=()\n'+base['BASELINE_CODE']+'''
original_baseline=baseline
def baseline():
 value=original_baseline()
 entries=list(Path('/dev/shm').iterdir());assert len(entries)<=256
 rows=[]
 for path in sorted(entries):
  info=path.lstat();rows.append(dict(name=path.name,dev=info.st_dev,ino=info.st_ino,bytes=info.st_size,mode=info.st_mode))
 value['parentNamespace']['shmInventory']=rows
 return value
''')


def main():
    parser = argparse.ArgumentParser(); parser.add_argument('--freeze', action='store_true')
    parser.add_argument('--execute-approved', action='store_true'); parser.add_argument('--manifest-sha256')
    args = parser.parse_args(); assert not (args.freeze and args.execute_approved)
    files, manifest, base = prepare()
    encoded = (json.dumps(manifest, sort_keys=True, indent=2)+'\n').encode()
    if args.freeze:
        with (HERE/'input-manifest.json').open('xb') as out: out.write(encoded)
    else: assert (HERE/'input-manifest.json').read_bytes() == encoded, 'fixed board input drift'
    digest = sha(encoded)
    if args.manifest_sha256 is not None: assert digest == args.manifest_sha256
    plan = dict(host=HOST, root=ROOT, manifestSha256=digest, remoteInputs=len(files),
                cases=manifest['cases'], executionAuthorized=False, connects=args.execute_approved)
    prelude = baseline_code(base)
    preflight = (files['environment.py'].decode()+prelude+'\nprint(json.dumps(dict(environment=inspect(),baseline=baseline())))\n')
    ast.parse(preflight, feature_version=(3, 6))
    if not args.execute_approved: print(json.dumps(plan, indent=2)); return 0
    assert args.manifest_sha256 is not None
    receipt = HERE/'board-23-B1-result.json'
    with receipt.open('x') as out: out.write('{"phase":"LOCAL_RESERVED","sshStarted":false}\n')
    result = dict(plan, sshStarted=False); client = None
    try:
        spec = importlib.util.spec_from_file_location('r10_board_connect', COORD/'run_bounded_probe.py')
        connection = importlib.util.module_from_spec(spec); spec.loader.exec_module(connection)
        client = connection.connect(HOST); result['sshStarted'] = True
        status, raw, err = base['invoke'](client, preflight, 60)
        result.update(preflightExitCode=status, preflightStdout=raw.decode(), preflightStderr=err.decode())
        assert status == 0 and not err, 'admission failed BEFORE upload'
        expected = json.loads(raw)
        with client.open_sftp() as sftp:
            sftp.get_channel().settimeout(30)
            parent = sftp.lstat('/tmp'); assert stat.S_ISDIR(parent.st_mode) and parent.st_uid == 0 and stat.S_IMODE(parent.st_mode) == 0o1777
            try: sftp.lstat(ROOT)
            except FileNotFoundError: pass
            else: raise ValueError('fresh board root occupied; no replay')
            sftp.mkdir(ROOT, mode=0o700)
            for directory in ('bin', 'source'): sftp.mkdir(ROOT+'/'+directory, mode=0o700)
            for name, data in files.items():
                row = manifest['remoteFiles'][name]; path = ROOT+'/'+row['directory']+'/'+name
                with sftp.open(path, 'wx') as out: out.write(data)
                sftp.chmod(path, row['mode'])
        code = prelude+'''
import hashlib,traceback
root=Path(ROOT)
try:
 code=(root/'source/board-run.py').read_bytes()
 assert hashlib.sha256(code).hexdigest()==MANIFEST['remoteFiles']['board-run.py']['sha256']
 exec(compile(code,'board-run.py','exec'))
 result=run(ROOT,MANIFEST,baseline,EXPECTED['baseline'],EXPECTED['environment'])
except BaseException as error:
 result=dict(primaryError=dict(type=type(error).__name__,message=str(error),traceback=traceback.format_exc(limit=10)))
print(json.dumps(result))
raise SystemExit(0 if 'accepted' in globals() and accepted(result) else 1)
'''
        code = 'ROOT='+repr(ROOT)+'\nMANIFEST='+repr(manifest)+'\nEXPECTED='+repr(expected)+'\n'+code
        ast.parse(code, feature_version=(3, 6))
        status, raw, err = base['invoke'](client, code, 420)
        result.update(remoteExitCode=status, stdout=raw.decode('utf-8', 'replace'), stderr=err.decode('utf-8', 'replace'))
        if raw: result['execution'] = json.loads(raw)
        with client.open_sftp() as sftp:
            sftp.get_channel().settimeout(30)
            for name in ('result.json', 'fixture.log'):
                try: info = sftp.lstat(ROOT+'/'+name)
                except FileNotFoundError: continue
                assert stat.S_ISREG(info.st_mode) and info.st_size <= 1024**2
                with sftp.open(ROOT+'/'+name, 'rb') as stream: data = stream.read(info.st_size+1)
                assert len(data) == info.st_size
                with (HERE/('B1-remote-'+name)).open('xb') as out: out.write(data)
    except BaseException as error:
        result['localError'] = dict(type=type(error).__name__, message=str(error))
    finally:
        try:
            if client is not None: client.close()
            result['sshClosed'] = True
        except BaseException as error: result.update(sshClosed=False, closureError=str(error))
        receipt.write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps({k:v for k,v in result.items() if k not in ('stdout','execution')}))
    return 0 if result.get('remoteExitCode') == 0 and result.get('sshClosed') and not result.get('stderr') and not any(k in result for k in ('localError','closureError')) else 1


if __name__ == '__main__': raise SystemExit(main())
