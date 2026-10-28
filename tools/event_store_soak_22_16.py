#!/usr/bin/env python3
"""Prepare a pinned 22.16 lab bundle offline; explicitly execute or collect later.

Never writes production binaries/configuration. SSH is confined to execute/collect.
Frozen source tools, verified cross-build artifacts and private SQLite are required.
"""
import argparse
import csv
import hashlib
import html
import io
import json
import os
from pathlib import Path, PurePosixPath
import platform
import re
import shlex
import signal
import shutil
import sqlite3
import subprocess
import sys
import tarfile
import tempfile
import time
import uuid

sys.dont_write_bytecode = True
MIGRATION_TOOLS = ('event_store_migrate.py', 'event_store_migrate_test.py', 'event_store_migrate_history_test.py',
                   'event_history_query.py', 'event_history_query_test.py')
MIGRATION_PROBE = 'event_store_migrate_ipc_test'
MIGRATION_SOURCE = 'mqtt_event_outbox.cpp'
PYTHON_MATRIX_MINIMA = {'event_store_migrate_test': 36, 'event_store_migrate_history_test': 10, 'event_history_query_test': 1}
PYTHON_MATRIX_TIMEOUT = 600
CAPACITY_BINARY = 'event_store_capacity_test'
CAPACITY_CASE = {'name': 'capacity-real-enospc', 'argv': ['@LAB@/event_store_capacity_test', '--enospc'],
                 'timeoutSeconds': 180, 'privateTmpfsBytes': 16 * 1024 * 1024}
CONFIG_EXAMPLES = {
    'config-example-mqtt-forward-disabled.json': 'config/examples/mqtt-forward-disabled.json',
    'config-runtime-mqtt-service.json': 'config/runtime/apps/mqtt-service.json',
    'config-factory-mqtt-service.json': 'config/factory/runtime/apps/mqtt-service.json',
}
TOOLS = ('event_store_soak.py', 'event_store_soak_journal.py', 'event_store_soak_state.py', 'event_store_soak_library.py',
         'event_store_soak_report.py', 'event_store_delivery_ipc_test.py',
         'event_store_soak_22_16.sh', 'event_store_soak_22_16.py') + MIGRATION_TOOLS
SERVICES = ('mqtt-driver@mqtt-service.service', 'mqtt-forwarder@mqtt-service.service',
            'event-engine@mqtt-service.service')
LAB_PARENT = '/opt/modbus-gateway/test-lab'


def require(ok, message):
    if not ok:
        raise ValueError(message)


def sha(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as f:
        for block in iter(lambda: f.read(1048576), b''):
            h.update(block)
    return h.hexdigest()


def save(path, obj):
    with Path(path).open('x', encoding='utf-8') as f:
        json.dump(obj, f, ensure_ascii=False, indent=2)
        f.write('\n')


def read(path):
    return json.loads(Path(path).read_text(encoding='utf-8'))


def fresh(path):
    path = Path(path).resolve()
    path.mkdir(parents=True, exist_ok=False)
    return path


def safe(name):
    p = PurePosixPath(name)
    return bool(name) and not p.is_absolute() and '..' not in p.parts and '\\' not in name and ':' not in name and str(p) == name


def lab_path(path):
    p = Path(path)
    require(re.fullmatch(re.escape(LAB_PARENT) + r'/event-store-soak-[A-Za-z0-9-]+', str(p)) is not None,
            'not a private lab directory')
    require(p.resolve(strict=True) == p and not p.is_symlink(), 'lab path is not canonical')
    return p


def matrix_contract(cmake, artifacts):
    source = re.sub(r'#[^\n]*', '', cmake)
    cases = []
    for match in re.finditer(r'\badd_test\s*\(([^()]*)\)', source, re.S):
        words = shlex.split(match[1])
        if len(words) < 4 or words[0] != 'NAME' or words[2] != 'COMMAND':
            continue
        executable = words[3]
        if executable not in artifacts or artifacts[executable]['kind'] != 'exe':
            continue
        argv = []
        for word in words[3:]:
            ref = re.fullmatch(r'\$<TARGET_FILE:([A-Za-z0-9_]+)>', word)
            if ref:
                choices = [ref[1], 'lib' + ref[1] + '.so', 'lib' + ref[1] + '.a']
                name = next((n for n in choices if n in artifacts), None)
                require(name is not None, 'missing matrix artifact: ' + word)
                argv.append('@LAB@/' + name)
            elif word in artifacts:
                argv.append('@LAB@/' + word)
            else:
                require('$' not in word and not word.startswith('/'), 'unresolved CTest argument: ' + word)
                argv.append(word)
        # These two tests choose their real SQLite via a positional argument.
        if executable in ('event_store_test', 'event_store_delivery_test'):
            require(len(argv) == 2, 'changed SQLite positional contract: ' + executable)
            argv.append('@LAB@/libsqlite3.so')
        require(re.fullmatch(r'[A-Za-z0-9_-]+', words[1]) is not None, 'unsafe CTest name')
        cases.append({'name': words[1], 'argv': argv, 'timeoutSeconds': 180})
    require(cases and len({c['name'] for c in cases}) == len(cases), 'empty/duplicate matrix')
    covered = {Path(c['argv'][0]).name for c in cases}
    return cases, sorted(n for n, v in artifacts.items() if v['kind'] == 'exe' and n not in covered)


def prepare(args):
    import event_store_cross_build as cross
    manifest = cross.verify(args.snapshot, args.manifest_sha256)
    require(set(CONFIG_EXAMPLES.values()) == set(cross.CONFIG_EXAMPLES), 'explicit config whitelist differs from snapshot policy')
    build_file = args.build_output / 'build-result.json'
    require(sha(build_file) == args.build_result_sha256, 'build result SHA mismatch')
    build = read(build_file)
    require(build['success'] and build['manifestSha256'] == args.manifest_sha256, 'build is not from pinned frozen source')
    expected = {t['artifact'] for t in manifest['targets'].values()}
    require(set(build['artifacts']) == expected, 'cross artifact inventory incomplete')
    require(build['artifacts'].get(MIGRATION_PROBE, {}).get('kind') == 'exe', 'migration matrix requires compiled event_store_migrate_ipc_test')
    require(build['artifacts'].get(CAPACITY_BINARY, {}).get('kind') == 'exe', 'capacity matrix requires compiled event_store_capacity_test')
    require(sha(args.sqlite_library) == args.sqlite_sha256, 'private SQLite SHA mismatch')
    cross.check_artifact(args.sqlite_library, 'shared')
    require(args.purpose == 'tool-selftest' or args.suite == 'soak', 'matrix/all requires --purpose tool-selftest')
    if args.duration is not None:
        require(0 < args.duration < float('inf'), 'duration must be finite and positive')
    require(0 < args.rate < float('inf'), 'rate must be finite and positive')
    require(0 < args.rpc_timeout < float('inf'), 'RPC timeout must be finite and positive')
    out = fresh(args.output)
    inputs = fresh(out / 'inputs')
    for name, meta in build['artifacts'].items():
        path = args.build_output / 'binaries' / name
        require(safe(name) and '/' not in name and sha(path) == meta['sha256'], 'artifact SHA/path mismatch: ' + name)
        cross.check_artifact(path, meta['kind'])
        (inputs / name).write_bytes(path.read_bytes())
    with tarfile.open(args.snapshot / 'source.tar.gz') as archive:
        cmake = archive.extractfile('CMakeLists.txt').read().decode('utf-8-sig')
        for name in TOOLS:
            data = archive.extractfile('tools/' + name).read()
            (inputs / name).write_bytes(data)
        (inputs / MIGRATION_SOURCE).write_bytes(archive.extractfile('src/' + MIGRATION_SOURCE).read())
        for flat, original in CONFIG_EXAMPLES.items():
            data = archive.extractfile(original).read()
            cross.validate_config_example(data)
            (inputs / flat).write_bytes(data)
    require(sha(inputs / Path(__file__).name) == sha(__file__), 'transport runner differs from frozen source')
    for name in ('libsqlite3.so', 'libsqlite3.so.0'):
        (inputs / name).write_bytes(args.sqlite_library.read_bytes())
    cases, uncovered = matrix_contract(cmake, build['artifacts'])
    save(inputs / 'matrix.json', {'cases': cases, 'notRunAsCTest': uncovered,
                                'additionalMatrix': [{'name': 'python-migration-history-query', 'minimumTestsByModule': PYTHON_MATRIX_MINIMA,
                                                      'probe': MIGRATION_PROBE, 'skipAllowed': False}, CAPACITY_CASE],
                                'usedByAdditionalMatrix': [MIGRATION_PROBE, CAPACITY_BINARY], 'accepted24h': False})
    duration = args.duration if args.duration is not None else (86400 if args.purpose == 'soak' else 60)
    require(duration > 0 and args.rate > 0, 'duration/rate must be positive')
    profiles = args.profile or ['wal-full']
    runs = []
    if args.suite in ('matrix', 'all'):
        runs.append({'suite': 'matrix', 'purpose': 'tool-selftest', 'timeout': sum(c['timeoutSeconds'] + 5 for c in cases) + PYTHON_MATRIX_TIMEOUT + 180 + 120})
    if args.suite in ('soak', 'all'):
        for profile in profiles:
            runs.append({'suite': 'soak', 'purpose': args.purpose, 'duration': duration, 'rate': args.rate,
                         'profile': profile, 'rpcTimeoutSeconds': args.rpc_timeout, 'timeout': duration + max(300, 4 * args.rpc_timeout + 180)})
    save(inputs / 'request.json', {'runs': runs, 'collectDatabases': args.collect_databases,
                                 'sourceManifestSha256': args.manifest_sha256,
                                 'buildResultSha256': args.build_result_sha256,
                                 'privateSqliteSha256': args.sqlite_sha256, 'productionServices': SERVICES,
                                 'configFixtures': {flat: dict(manifest['files'][original], sourcePath=original)
                                                    for flat, original in CONFIG_EXAMPLES.items()}})
    sums = {p.name: sha(p) for p in sorted(inputs.iterdir())}
    (inputs / 'SHA256SUMS').write_bytes(''.join(v + '  ' + n + '\n' for n, v in sums.items()).encode('ascii'))
    sums['SHA256SUMS'] = sha(inputs / 'SHA256SUMS')
    with tarfile.open(out / 'bundle.tar.gz', 'x:gz') as archive:
        for p in sorted(inputs.iterdir()):
            data = p.read_bytes()
            info = tarfile.TarInfo(p.name)
            info.size, info.mode, info.mtime = len(data), 0o755 if p.name in build['artifacts'] or p.suffix == '.sh' else 0o644, 0
            archive.addfile(info, io.BytesIO(data))
    contract = {'files': sums, 'archiveSha256': sha(out / 'bundle.tar.gz'), 'request': read(inputs / 'request.json'),
                'sourceManifestSha256': args.manifest_sha256, 'buildResultSha256': args.build_result_sha256,
                'scope': 'isolated lab only; not deployment', 'remoteConnections': 0}
    save(out / 'bundle.json', contract)
    return {'bundle': str(out), 'bundleSha256': sha(out / 'bundle.json'), 'matrixCases': len(cases), 'notRunAsCTest': uncovered, 'remoteConnections': 0}


def verify_bundle(bundle, expected):
    require(sha(bundle / 'bundle.json') == expected, 'bundle contract SHA mismatch')
    contract = read(bundle / 'bundle.json')
    require(sha(bundle / 'bundle.tar.gz') == contract['archiveSha256'], 'bundle archive SHA mismatch')
    seen = set()
    with tarfile.open(bundle / 'bundle.tar.gz') as archive:
        for item in archive:
            require(item.isfile() and safe(item.name) and '/' not in item.name and item.name not in seen, 'unsafe bundle member')
            seen.add(item.name)
            require(hashlib.sha256(archive.extractfile(item).read()).hexdigest() == contract['files'].get(item.name), 'bundle member SHA mismatch')
            if item.name == 'request.json':
                require(json.loads(archive.extractfile(item).read()) == contract['request'], 'bundle request contract mismatch')
    require(seen == set(contract['files']), 'bundle inventory mismatch')
    return contract


def verify_inputs(lab):
    seen = set()
    for line in (lab / 'SHA256SUMS').read_text(encoding='ascii').splitlines():
        match = re.fullmatch(r'([0-9a-f]{64})  ([A-Za-z0-9_.-]+)', line)
        require(match is not None, 'unsafe SHA256SUMS entry')
        value, name = match.groups()
        require(name not in seen and not (lab / name).is_symlink() and sha(lab / name) == value, 'input SHA mismatch: ' + name)
        seen.add(name)
    require(set(TOOLS) | set(CONFIG_EXAMPLES) | {'EventStore', 'request.json', 'matrix.json', 'libsqlite3.so', 'libsqlite3.so.0', MIGRATION_PROBE, MIGRATION_SOURCE, CAPACITY_BINARY} <= seen, 'missing signed input')


def process_snapshot():
    services = {}
    for unit in SERVICES:
        raw = subprocess.check_output(['systemctl', 'show', unit, '--no-pager', '-p', 'Id', '-p', 'LoadState',
                                       '-p', 'ActiveState', '-p', 'MainPID', '-p', 'NRestarts'], text=True)
        fields = dict(line.split('=', 1) for line in raw.splitlines() if '=' in line)
        pid = int(fields['MainPID'])
        require(fields['Id'] == unit and fields['LoadState'] == 'loaded' and fields['ActiveState'] == 'active' and pid > 0,
                'production service not active: ' + unit)
        proc = Path('/proc') / str(pid)
        fields.update(startTicks=(proc / 'stat').read_text().rsplit(')', 1)[1].split()[19], executable=os.readlink(proc / 'exe'))
        services[unit] = fields
    processes = {}
    for p in Path('/proc').iterdir():
        if not p.name.isdigit():
            continue
        try:
            exe = os.readlink(p / 'exe')
            if exe.startswith('/opt/modbus-gateway/') and not exe.startswith(LAB_PARENT + '/'):
                processes[p.name] = {'executable': exe, 'startTicks': (p / 'stat').read_text().rsplit(')', 1)[1].split()[19]}
        except FileNotFoundError:
            continue
    return {'bootId': Path('/proc/sys/kernel/random/boot_id').read_text().strip(), 'services': services, 'processes': processes}


def same_production(before, after):
    require(set(before['services']) == set(SERVICES) and set(after['services']) == set(SERVICES), 'production roster incomplete')
    require(before == after, 'production PID/startTicks/boot/service/process roster changed')


def bounded(argv, log, timeout, cwd, env=None):
    with Path(log).open('xb') as f:
        p = subprocess.Popen(argv, stdout=f, stderr=subprocess.STDOUT, cwd=cwd, env=env, start_new_session=True)
        try:
            return p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
            return 124
        finally:
            # A passing test must not leave background children in its process group.
            try:
                os.killpg(p.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass


def human_report(output, result, rows, uncovered):
    title = 'EventStore 22.16 隔离实验报告'
    conclusion = '本轮实验通过' if result.get('passed') else '本轮实验未通过或证据不完整'
    if result.get('state') == 'running':
        conclusion = '后台实验运行中，尚无通过结论'
    lines = ['# ' + title, '', '## 关键结论', '', conclusion + '。本汇总不授予 24 小时验收；时长结论见各 full 报告。', '',
             '| 指标 | 实测值 | 判定 | 单位 |', '| --- | --- | --- | --- |']
    lines += ['| ' + ' | '.join(str(v).replace('|', '\\|').replace('\n', ' ') for v in row) + ' |' for row in rows]
    lines += ['', '## 未覆盖项', ''] + ['- ' + item for item in uncovered]
    (output / 'report.md').write_text('\n'.join(lines) + '\n', encoding='utf-8')
    with (output / 'report.csv').open('x', encoding='utf-8', newline='') as stream:
        writer = csv.writer(stream)
        writer.writerow(['指标', '实测值', '判定', '单位'])
        writer.writerows(rows)
    table = '<table><thead><tr>' + ''.join('<th>' + x + '</th>' for x in ('指标', '实测值', '判定', '单位')) + '</tr></thead><tbody>'
    table += ''.join('<tr>' + ''.join('<td>' + html.escape(str(v)) + '</td>' for v in row) + '</tr>' for row in rows) + '</tbody></table>'
    (output / 'report.html').write_text('<!doctype html><html lang="zh-CN"><meta charset="utf-8"><title>' + title + '</title><style>body{font:16px sans-serif;margin:32px;line-height:1.6}table{border-collapse:collapse;width:100%}th,td{border:1px solid #bbb;padding:8px;text-align:left;overflow-wrap:anywhere}th{background:#eee}</style><h1>' + title + '</h1><h2>关键结论</h2><p>' + conclusion + '。本汇总不授予24小时验收。</p>' + table + '<h2>未覆盖项</h2><ul>' + ''.join('<li>' + html.escape(x) + '</li>' for x in uncovered) + '</ul></html>', encoding='utf-8')
    save(output / 'report.json', result)


PYTHON_UNITTEST_RUNNER = '''import json,os,pathlib,sys,unittest
minima=json.loads(sys.argv[2])
probe=pathlib.Path(os.environ['EVENT_STORE_MIGRATE_PROBE'])
if not probe.is_file() or probe.is_symlink() or not os.access(probe,os.X_OK):
 raise RuntimeError('compiled migration probe missing or not executable; skipping forbidden')
loader=unittest.TestLoader()
suite=unittest.TestSuite()
counts={}
for module,minimum in minima.items():
 tests=loader.loadTestsFromName(module)
 counts[module]=tests.countTestCases()
 if counts[module]<minimum: raise RuntimeError('incomplete unittest module: '+module)
 suite.addTests(tests)
if loader.errors: raise RuntimeError('unittest discovery errors: '+str(loader.errors))
class Result(unittest.TextTestResult):
 def __init__(self,*args,**kwargs):
  super().__init__(*args,**kwargs); self.successIds=[]
 def addSuccess(self,test):
  super().addSuccess(test); self.successIds.append(test.id())
result=unittest.TextTestRunner(verbosity=2,resultclass=Result).run(suite)
probe_id='event_store_migrate_history_test.HistoryTests.test_real_cpp_client_empty_id_migrate_ack_rollback_and_projection'
passed=result.wasSuccessful() and not result.skipped and probe_id in result.successIds
report=dict(passed=passed,accepted24h=False,testsRun=result.testsRun,discoveredByModule=counts,
 successes=result.successIds,skipped=[(t.id(),reason) for t,reason in result.skipped],
 failures=[(t.id(),detail) for t,detail in result.failures],errors=[(t.id(),detail) for t,detail in result.errors],
 probeTestId=probe_id,probeTestPassed=probe_id in result.successIds,probe=str(probe))
with open(sys.argv[1],'x',encoding='utf-8') as stream: json.dump(report,stream,ensure_ascii=False,indent=2)
sys.exit(0 if passed else 1)
'''


def run_python_matrix(lab, output, env):
    """Use the same bundled tests on device and in the native wrapper selftest."""
    probe = lab / MIGRATION_PROBE
    require(probe.is_file() and not probe.is_symlink() and os.access(probe, os.X_OK),
            'migration matrix requires an executable regular lab probe; skipping forbidden')
    expected_probe_sha = sha(probe)
    name = 'python-migration-history-query'
    report_path = output / (name + '.json')
    start = time.monotonic()
    # Native Linux locking is required (WSL's Windows mount mixes flock/SQLite locks).
    # On the board /var/tmp is already a private namespace tmpfs; logs stay in output.
    with tempfile.TemporaryDirectory(prefix='event-store-python-matrix-', dir='/var/tmp') as directory:
        workspace = Path(directory)
        (workspace / 'tools').mkdir()
        (workspace / 'src').mkdir()
        (workspace / 'tmp').mkdir()
        for file in MIGRATION_TOOLS:
            shutil.copyfile(lab / file, workspace / 'tools' / file)
        shutil.copyfile(lab / MIGRATION_SOURCE, workspace / 'src' / MIGRATION_SOURCE)
        environment = dict(env, EVENT_STORE_MIGRATE_PROBE=str(probe.resolve()),
                           TMPDIR=str(workspace / 'tmp'), PYTHONPATH=str(workspace / 'tools'), PYTHONDONTWRITEBYTECODE='1')
        argv = [sys.executable, '-B', '-c', PYTHON_UNITTEST_RUNNER, str(report_path.resolve()), json.dumps(PYTHON_MATRIX_MINIMA)]
        code = bounded(argv, output / (name + '.log'), PYTHON_MATRIX_TIMEOUT, workspace / 'tools', environment)
    detail = read(report_path) if report_path.exists() else {'passed': False, 'error': 'unittest result missing; inspect log (discovery/crash/timeout)'}
    return dict(name=name, category='additional-python-unittest-matrix', exitCode=code,
                actualSeconds=time.monotonic() - start, passed=code == 0 and detail['passed'] and sha(probe) == expected_probe_sha,
                probeSha256=expected_probe_sha, probe=str(probe), unittest=detail, accepted24h=False)


def materialize_config_examples(lab, workspace):
    for flat, relative in CONFIG_EXAMPLES.items():
        target = workspace / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        with target.open('xb') as stream:
            stream.write((lab / flat).read_bytes())


def run_capacity_matrix(lab, output, env):
    binary = lab / CAPACITY_BINARY
    require(binary.is_file() and not binary.is_symlink(), 'missing regular capacity executable')
    log = output / 'capacity-real-enospc.log'
    start = time.monotonic()
    # The C++ --enospc branch mounts its own 16 MiB tmpfs inside the wrapper namespace.
    code = bounded([str(binary), '--enospc'], log, 180, lab,
                   dict(env, SQLITE_LIBRARY=str(lab / 'libsqlite3.so'), LD_LIBRARY_PATH=str(lab)))
    lines = log.read_text(encoding='utf-8', errors='replace').splitlines()
    checks = {profile: lines.count('PASS real-ENOSPC-' + profile + '-recovery') == 1 for profile in ('delete', 'wal')}
    return dict(CAPACITY_CASE, exitCode=code, actualSeconds=time.monotonic() - start,
                passed=code == 0 and all(checks.values()), recoveryChecks=checks,
                binarySha256=sha(binary), privateSqliteSha256=sha(lab / 'libsqlite3.so'))


def matrix_local(args):
    lab = lab_path(args.lab)
    parent = json.loads(args.parent_namespaces)
    require(set(parent) == {'mnt', 'pid', 'ipc', 'net'} and all(os.readlink('/proc/self/ns/' + n) != v for n, v in parent.items()), 'matrix requires fresh namespaces')
    verify_inputs(lab)
    out = fresh(args.output)
    require(out.parent.parent == lab and out.parent.name.startswith('run-'), 'matrix output outside private run')
    matrix = read(lab / 'matrix.json')
    env = dict(os.environ, SQLITE_LIBRARY=str(lab / 'libsqlite3.so'), LD_LIBRARY_PATH=str(lab), TMPDIR='/tmp')
    cases = []
    for case in matrix['cases']:
        argv = [a.replace('@LAB@', str(lab)) for a in case['argv']]
        require(Path(argv[0]).parent == lab and not Path(argv[0]).is_symlink(), 'matrix executable outside lab')
        start = time.monotonic()
        if Path(argv[0]).name == 'config_loader_test':
            with tempfile.TemporaryDirectory(prefix='config-matrix-', dir=str(out)) as directory:
                workspace = Path(directory)
                materialize_config_examples(lab, workspace)
                code = bounded(argv, out / (case['name'] + '.log'), case['timeoutSeconds'], workspace, env)
        else:
            code = bounded(argv, out / (case['name'] + '.log'), case['timeoutSeconds'], lab, env)
        cases.append(dict(case, exitCode=code, actualSeconds=time.monotonic() - start, passed=code == 0))
    capacity_result = run_capacity_matrix(lab, out, env)
    cases.append(capacity_result)
    cases.append(run_python_matrix(lab, out, env))
    result = {'purpose': 'tool-selftest', 'suite': 'matrix', 'passed': all(c['passed'] for c in cases), 'accepted24h': False, 'cases': cases,
              'privateSqliteSha256': sha(lab / 'libsqlite3.so'), 'namespaces': {n: os.readlink('/proc/self/ns/' + n) for n in parent},
              'configFixtures': read(lab / 'request.json')['configFixtures']}
    python_result = cases[-1]['unittest']
    rows = [(c['name'], c['exitCode'], '通过' if c['passed'] else '失败', '退出码') for c in cases]
    rows += [('额外 Python unittest 执行数', python_result.get('testsRun'), '迁移/历史至少46项，加query至少1项', '项'),
             ('额外 Python unittest 跳过数', len(python_result.get('skipped', [])), '任何skip都失败', '项'),
             ('真实迁移 IPC 探针用例', python_result.get('probeTestPassed', False), '必须成功，禁止skip', '判定')]
    rows += [('真实 ENOSPC ' + profile.upper() + ' 恢复', ok, '通过' if ok and capacity_result['passed'] else '失败', '判定')
             for profile, ok in capacity_result['recoveryChecks'].items()]
    uncovered = [n for n in matrix['notRunAsCTest'] if n not in matrix.get('usedByAdditionalMatrix', [])]
    human_report(out, result, rows,
                 ['未直接作为CTest运行：' + ', '.join(uncovered), '未覆盖设备掉电、24 小时持续负载和真实 broker。', '夹具测试按 CTest 参数故意使用故障库；不声称全部用例都加载真实 SQLite。'])
    return result


def backup(source, destination):
    from event_store_soak_journal import backup as sqlite_backup
    sqlite_backup(source, destination)


def collect_local(lab, include_databases):
    evidence = fresh(lab / 'return-files')
    files = [p for p in lab.glob('run-*/**/*') if p.is_file()]
    files += [p for p in lab.iterdir() if p.is_file() and (p.name.startswith('host-') or p.name in ('request.json', 'matrix.json', 'SHA256SUMS'))]
    for source in sorted(files):
        rel = source.relative_to(lab)
        require(not source.is_symlink() and lab in source.resolve().parents, 'unsafe evidence path')
        if source.suffix in ('.db', '.sqlite'):
            if not include_databases:
                continue
            dest = evidence / rel
            dest.parent.mkdir(parents=True, exist_ok=True)
            backup(source, dest)
        elif source.suffix in ('.md', '.html', '.json', '.jsonl', '.csv', '.log', '.txt') or source.name == 'SHA256SUMS':
            dest = evidence / rel
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(source.read_bytes())
    hashes = {p.relative_to(evidence).as_posix(): sha(p) for p in sorted(evidence.rglob('*')) if p.is_file()}
    save(lab / 'return.json', {'files': hashes, 'databaseMethod': 'read-only SQLite backup; DB/WAL source not raw copied', 'includesDatabases': include_databases})
    with tarfile.open(lab / 'return.tar.gz', 'x:gz') as tar:
        for name in hashes:
            tar.add(evidence / name, arcname=name, recursive=False)
    save(lab / 'return-sha.json', {'archiveSha256': sha(lab / 'return.tar.gz'), 'manifestSha256': sha(lab / 'return.json')})


def host_run(args):
    lab = lab_path(args.lab)
    require(os.geteuid() == 0 and platform.machine() == 'aarch64', 'host-run requires root AArch64')
    verify_inputs(lab)
    request = read(lab / 'request.json')
    require(tuple(request['productionServices']) == SERVICES, 'production service contract mismatch')
    save(lab / 'host-attempt.json', {'startedUnix': time.time()})
    result = {'passed': False, 'accepted24h': False, 'runs': [], 'productionUnchanged': False}
    before = None
    try:
        before = process_snapshot()
        save(lab / 'host-production-before.json', before)
        for i, run in enumerate(request['runs']):
            argv = ['bash', str(lab / 'event_store_soak_22_16.sh'), '--execute', str(lab), sha(lab / 'EventStore'),
                    '--private-sqlite', request['privateSqliteSha256'], '--purpose', run['purpose'], '--suite', run['suite']]
            if run['suite'] == 'soak':
                argv += ['--duration', str(run['duration']), '--rate', str(run['rate']), '--profile', run['profile'],
                         '--rpc-timeout', str(run['rpcTimeoutSeconds'])]
            code = bounded(argv, lab / ('host-run-' + str(i) + '.log'), run['timeout'], lab)
            result['runs'].append(dict(run, exitCode=code))
        result['passed'] = all(r['exitCode'] == 0 for r in result['runs'])
    except Exception as exc:
        result['error'] = str(exc)
    finally:
        try:
            after = process_snapshot()
            save(lab / 'host-production-after.json', after)
            require(before is not None, 'production baseline missing')
            same_production(before, after)
            result['productionUnchanged'] = True
        except Exception as exc:
            result['passed'] = False
            result['productionError'] = str(exc)
        result['state'] = 'completed' if result['passed'] else 'failed'
        save(lab / 'host-result.json', result)
        try:
            collect_local(lab, request['collectDatabases'])
        except Exception as exc:
            save(lab / 'host-collection-error.json', {'error': str(exc)})
            raise
    return result


def detach_allowed(request):
    require(request['runs'] and all(r['suite'] == 'soak' and r['purpose'] == 'soak' and r['duration'] >= 86400
                                    for r in request['runs']), '--detach requires only soak-purpose runs of at least 86400 seconds')


def process_identity(pid):
    proc = Path('/proc') / str(pid)
    stat = (proc / 'stat').read_text().rsplit(')', 1)[1].split()
    return {'pid': pid, 'startTicks': stat[19], 'processState': stat[0],
            'bootId': Path('/proc/sys/kernel/random/boot_id').read_text().strip(),
            'argv': (proc / 'cmdline').read_bytes().decode().rstrip('\0').split('\0')}


def launch_session(argv, cwd, log):
    # Fixed host-run is passed by launch_detached; no inherited SSH stdin/stdout or process group.
    old = signal.signal(signal.SIGHUP, signal.SIG_IGN)
    try:
        with Path(log).open('xb') as stream:
            child = subprocess.Popen(argv, cwd=cwd, stdin=subprocess.DEVNULL, stdout=stream,
                                     stderr=subprocess.STDOUT, start_new_session=True, close_fds=True)
        identity = process_identity(child.pid)
        identity['argv'] = argv
        return child, identity
    finally:
        signal.signal(signal.SIGHUP, old)


def launch_detached(args):
    lab = lab_path(args.lab)
    require(os.geteuid() == 0 and platform.machine() == 'aarch64', 'detached launch requires root AArch64')
    verify_inputs(lab)
    detach_allowed(read(lab / 'request.json'))
    require(not (lab / 'host-attempt.json').exists(), 'host-run already attempted; refusing restart')
    save(lab / 'host-launch-claim.json', {'claimedUnix': time.time()})
    argv = [sys.executable, '-B', str(lab / 'event_store_soak_22_16.py'), 'host-run', '--lab', str(lab)]
    child, identity = launch_session(argv, lab, lab / 'host-detached.log')
    receipt = dict(identity, runId=uuid.uuid4().hex, lab=str(lab), launchedUnix=time.time(),
                   requestSha256=sha(lab / 'request.json'), runnerSha256=sha(lab / 'event_store_soak_22_16.py'))
    save(lab / 'host-launch.json', receipt)
    # Do not wait, poll for completion, or interpret launch as experiment success.
    return dict(state='running', passed=None, accepted24h=False, receipt=receipt)


def status_at(lab):
    result = dict(state='failed', passed=False, accepted24h=False, collectionReady=False, lab=str(lab))
    receipt_path = lab / 'host-launch.json'
    receipt = read(receipt_path) if receipt_path.exists() else None
    if receipt:
        result['receipt'] = receipt
        require(sha(lab / 'request.json') == receipt['requestSha256'] and sha(lab / 'event_store_soak_22_16.py') == receipt['runnerSha256'], 'detached input identity drift')
    live = False
    if receipt:
        try:
            current = process_identity(receipt['pid'])
            live = (all(current[k] == receipt[k] for k in ('pid', 'startTicks', 'bootId', 'argv')) and
                    current['processState'] not in ('Z', 'X'))
        except (OSError, ValueError):
            pass
    ready = (lab / 'return-sha.json').is_file()
    if ready and (lab / 'host-result.json').is_file():
        pins = read(lab / 'return-sha.json')
        require(sha(lab / 'return.tar.gz') == pins['archiveSha256'] and sha(lab / 'return.json') == pins['manifestSha256'], 'remote evidence bundle changed')
        host = read(lab / 'host-result.json')
        result.update(state='completed' if host['passed'] else 'failed', passed=host['passed'], collectionReady=True,
                      productionUnchanged=host['productionUnchanged'])
    elif live:
        result.update(state='running', passed=None, phase='collecting' if (lab / 'host-result.json').exists() else 'testing')
    else:
        result['error'] = 'no live matching PID/startTicks and no complete return bundle; do not restart this lab'
    return result


def status_local(args):
    return status_at(lab_path(args.lab))


def unpack_return(archive_path, manifest_path, out):
    hashes = read(manifest_path)['files']
    seen = set()
    with tarfile.open(archive_path) as tar:
        for item in tar:
            require(item.isfile() and safe(item.name) and item.name not in seen, 'unsafe return member')
            data = tar.extractfile(item).read()
            require(hashlib.sha256(data).hexdigest() == hashes.get(item.name), 'return file SHA mismatch')
            seen.add(item.name)
            target = out / item.name
            target.parent.mkdir(parents=True, exist_ok=True)
            with target.open('xb') as f:
                f.write(data)
    require(seen == set(hashes), 'return file inventory mismatch')


def remote(args):
    require(args.command == 'status' or args.execute, 'remote access requires --execute')
    if args.command == 'execute':
        contract = verify_bundle(args.bundle, args.bundle_sha256)
        if args.detach:
            detach_allowed(contract['request'])
    else:
        require(re.fullmatch(re.escape(LAB_PARENT) + r'/event-store-soak-[A-Za-z0-9-]+', args.remote_lab) is not None, 'invalid collection lab')
    import paramiko
    out = fresh(args.output)
    client = paramiko.SSHClient()
    client.load_system_host_keys()
    client.set_missing_host_key_policy(paramiko.RejectPolicy())
    result = {'passed': False, 'deployed': False, 'accepted24h': False, 'operations': []}

    def run(command, name, timeout=120, allow_failure=False):
        channel = client.get_transport().open_session(timeout=15)
        channel.set_combine_stderr(True)
        channel.exec_command(command)
        start, output = time.monotonic(), bytearray()
        try:
            with (out / (name + '.log')).open('xb') as log:
                while True:
                    while channel.recv_ready():
                        block = channel.recv(65536)
                        log.write(block); log.flush(); output.extend(block)
                    if channel.exit_status_ready() and not channel.recv_ready():
                        break
                    require(time.monotonic() - start < timeout, 'SSH timeout; remote run may still be active, do not rerun')
                    time.sleep(.1)
            code = channel.recv_exit_status()
            result['operations'].append({'name': name, 'exitCode': code})
            require(allow_failure or code == 0, 'remote operation failed: ' + name)
            return output.decode('utf-8', 'replace')
        finally:
            channel.close()

    q = shlex.quote
    try:
        client.connect('192.168.22.16', username=args.user, password=os.environ.get('GATEWAY_LAB_PASSWORD'), timeout=15, auth_timeout=15, banner_timeout=15)
        if args.command == 'execute':
            lab = LAB_PARENT + '/event-store-soak-' + time.strftime('%Y%m%d-%H%M%S', time.gmtime()) + '-' + uuid.uuid4().hex
            result['remoteLab'] = lab
            save(out / 'remote-location.json', {'remoteLab': lab, 'bundleSha256': args.bundle_sha256})
            run('test "$(uname -m)" = aarch64 && test "$(id -u)" = 0 && mkdir -- ' + q(lab), 'create-private-lab')
            with client.open_sftp() as sftp:
                sftp.put(str(args.bundle / 'bundle.tar.gz'), lab + '/bundle.tar.gz')
            actual = run('sha256sum -- ' + q(lab + '/bundle.tar.gz'), 'upload-sha').split()[0]
            require(actual == contract['archiveSha256'], 'remote uploaded archive SHA mismatch')
            run('tar --no-same-owner -xzf ' + q(lab + '/bundle.tar.gz') + ' -C ' + q(lab) + ' && cd ' + q(lab) + ' && sha256sum -c --strict SHA256SUMS', 'verify-private-inputs')
            if args.detach:
                launch = json.loads(run('python3 -B ' + q(lab + '/event_store_soak_22_16.py') + ' launch-detached --lab ' + q(lab), 'launch-detached'))
                result.update(launch)
                save(out / 'run-receipt.json', launch['receipt'])
                return result
            limit = sum(r['timeout'] for r in contract['request']['runs']) + 900
            run('python3 -B ' + q(lab + '/event_store_soak_22_16.py') + ' host-run --lab ' + q(lab), 'host-run', limit, allow_failure=True)
        else:
            lab = args.remote_lab
            result['remoteLab'] = lab
        state = json.loads(run('python3 -B ' + q(lab + '/event_store_soak_22_16.py') + ' status-local --lab ' + q(lab), 'status', allow_failure=True))
        if args.command == 'status':
            result.update(state)
            return result
        require(state['state'] in ('completed', 'failed') and state['collectionReady'], 'run not finished with complete evidence; use status, do not restart')
        with client.open_sftp() as sftp:
            for name in ('return-sha.json', 'return.json', 'return.tar.gz'):
                remote_hash = run('sha256sum -- ' + q(lab + '/' + name), 'download-sha-' + name.replace('.', '-')).split()[0]
                sftp.get(lab + '/' + name, str(out / name))
                require(sha(out / name) == remote_hash, 'download SHA mismatch: ' + name)
        pins = read(out / 'return-sha.json')
        require(sha(out / 'return.json') == pins['manifestSha256'] and sha(out / 'return.tar.gz') == pins['archiveSha256'], 'returned evidence contract mismatch')
        evidence = fresh(out / 'evidence')
        unpack_return(out / 'return.tar.gz', out / 'return.json', evidence)
        host = read(evidence / 'host-result.json')
        require(host['productionUnchanged'], 'production identity assertion failed')
        same_production(read(evidence / 'host-production-before.json'), read(evidence / 'host-production-after.json'))
        result['passed'] = host['passed']
        result['productionUnchanged'] = True
        result['state'] = 'completed' if result['passed'] else 'failed'
    except Exception as exc:
        result['error'] = str(exc)
        result['state'] = 'failed'
    finally:
        client.close()
        human_report(out, result, [('实验状态', result.get('state', 'failed'), '运行中无验收结论' if result.get('state') == 'running' else '完成状态', '状态'),
                                  ('生产 PID/startTicks 前后相同', result.get('productionUnchanged', False), '通过' if result.get('productionUnchanged') else '未证实', '断言'),
                                  ('传输及实验完整通过', result['passed'], '通过' if result['passed'] else '失败', '判定')],
                     ['不替换生产二进制，不授予生产验收；各场景指标见 evidence/run-*/result/report。', '24 小时只由实际 full soak 报告判定，matrix 永不 accepted24h。'])
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    p = sub.add_parser('prepare', help='offline bundle from a frozen snapshot and its verified cross-build')
    p.add_argument('--snapshot', type=Path, required=True)
    p.add_argument('--manifest-sha256', required=True)
    p.add_argument('--build-output', type=Path, required=True)
    p.add_argument('--build-result-sha256', required=True)
    p.add_argument('--sqlite-library', type=Path, required=True)
    p.add_argument('--sqlite-sha256', required=True)
    p.add_argument('--purpose', choices=['soak', 'tool-selftest'], default='soak')
    p.add_argument('--suite', choices=['soak', 'matrix', 'all'], default='soak')
    p.add_argument('--duration', type=float)
    p.add_argument('--rate', type=float, default=20)
    p.add_argument('--rpc-timeout', type=float, default=30)
    p.add_argument('--profile', choices=['delete-full', 'wal-full'], action='append')
    p.add_argument('--collect-databases', action='store_true')
    p.add_argument('--output', type=Path, required=True)
    for command in ('execute', 'collect', 'status'):
        p = sub.add_parser(command)
        p.add_argument('--execute', action='store_true')
        p.add_argument('--user', default='root')
        p.add_argument('--output', type=Path, required=True)
        if command == 'execute':
            p.add_argument('--bundle', type=Path, required=True)
            p.add_argument('--bundle-sha256', required=True)
            p.add_argument('--detach', action='store_true', help='only soak-purpose runs >=24h; start fixed host-run in a new session')
        else:
            p.add_argument('--remote-lab', required=True)
    p = sub.add_parser('host-run', help='internal: run on the lab host, with production identity assertions')
    p.add_argument('--lab', type=Path, required=True)
    for command in ('launch-detached', 'status-local'):
        p = sub.add_parser(command)
        p.add_argument('--lab', type=Path, required=True)
    p = sub.add_parser('matrix-local', help='internal: requires fresh namespaces from the wrapper')
    p.add_argument('--lab', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--parent-namespaces', required=True)
    args = parser.parse_args()
    local = {'prepare': prepare, 'host-run': host_run, 'matrix-local': matrix_local, 'launch-detached': launch_detached, 'status-local': status_local}
    result = local.get(args.command, remote)(args)
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 1 if result.get('passed') is False else 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except Exception as exc:
        print(type(exc).__name__ + ': ' + str(exc), file=sys.stderr)
        sys.exit(2)
