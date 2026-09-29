#!/usr/bin/env python3
"""Offline synthetic bundle/collection tests; never connects to or deploys on 22.16."""
import argparse
import copy
import json
import os
from pathlib import Path
import sqlite3
import struct
import subprocess
import sys
import time
import types

sys.dont_write_bytecode = True
import event_store_cross_build as cross
import event_store_soak_22_16 as lab


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--migration-probe', type=Path, required=True, help='existing native probe; no build is performed')
    parser.add_argument('--config-loader', type=Path, required=True, help='existing native config_loader_test')
    args = parser.parse_args()
    out = lab.fresh(args.output)
    cases = []
    def passed(name):
        cases.append(dict(name=name, passed=True))
    def rejects(name, fn):
        try:
            fn()
        except (ValueError, FileExistsError):
            passed(name)
        else:
            raise AssertionError('expected rejection: ' + name)
    wrapper = Path(lab.__file__).with_suffix('.sh')
    for options, purpose, suite, duration in (([], 'soak', 'soak', 86400),
            (['--purpose', 'tool-selftest', '--duration', '8'], 'tool-selftest', 'soak', 60),
            (['--purpose', 'tool-selftest', '--suite', 'matrix'], 'tool-selftest', 'matrix', 60)):
        command = ['bash', str(wrapper), '--inspect-options', '/not-accessed', '0' * 64] + options
        result = subprocess.run(command, capture_output=True, text=True, check=True)
        observed = json.loads(result.stdout)
        assert observed['purpose'] == purpose and observed['suite'] == suite and observed['defaultDuration'] == duration and not observed['execution']
        (out / ('wrapper-' + purpose + '-' + suite + '.log')).write_text(result.stdout, encoding='utf-8')
        passed('wrapper options ' + purpose + ' ' + suite)
    for options in (['--suite', 'matrix'], ['--purpose=tool-selftest'], ['--binary', '/tmp/x'], ['--sqlite-library', '/tmp/x']):
        p = subprocess.run(['bash', str(wrapper), '--inspect-options', '/unused', '0' * 64] + options, capture_output=True)
        assert p.returncode != 0
    passed('wrapper matrix purpose and owned-option gates')
    root = lab.fresh(out / 'source-fixture')
    (root / 'tools').mkdir()
    (root / 'toolchains').mkdir()
    libraries = {'edge_gateway', 'event_store_runtime', 'event_store_client', 'event_store_producer', 'event_store_sender', 'event_stats_reader'}
    fixtures = {'sqlite_failure_fixture', 'sqlite_outbox_failure_fixture'}
    cmake = []
    for target in sorted(cross.MINIMUM | {lab.MIGRATION_PROBE, lab.CAPACITY_BINARY}):
        kind = 'library' if target in libraries | fixtures else 'executable'
        modifier = 'SHARED ' if target in fixtures else ''
        cmake.append('add_' + kind + '(' + target + ' ' + modifier + 'tools/input.cpp)')
    cmake += ['add_test(NAME event_store COMMAND event_store_test $<TARGET_FILE:sqlite_outbox_failure_fixture>)',
              'add_test(NAME event_store_delivery COMMAND event_store_delivery_test $<TARGET_FILE:sqlite_outbox_failure_fixture>)',
              'add_test(NAME sqlite_outbox COMMAND sqlite_outbox_failure_test $<TARGET_FILE:sqlite_outbox_failure_fixture> $<TARGET_FILE:sqlite_failure_fixture>)',
              'add_test(NAME transport COMMAND event_store_transport_test)']
    (root / 'CMakeLists.txt').write_text('\n'.join(cmake), encoding='utf-8')
    (root / 'tools/input.cpp').write_text('int main() {return 0;}\n', encoding='utf-8')
    (root / 'tools/build_edge_aarch64.sh').write_text('#!/bin/bash\nexit 0\n', encoding='utf-8')
    (root / 'toolchains/aarch64-linux-gnu.cmake').write_text('set(CMAKE_SYSTEM_PROCESSOR aarch64)\n', encoding='utf-8')
    for name in cross.CONFIG_EXAMPLES:
        target = root / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(json.dumps({'mqttForward': {'enabled': False, 'broker': ''},
                                     'username': 'synthetic-user', 'password': 'synthetic-secret'}), encoding='utf-8')
    (root / 'config/do-not-package.json').write_text('{"password":"synthetic-unrelated-secret"}', encoding='utf-8')
    (root / 'src').mkdir()
    (root / 'src' / lab.MIGRATION_SOURCE).write_bytes(Path(lab.__file__).parents[1].joinpath('src', lab.MIGRATION_SOURCE).read_bytes())
    for name in (*lab.TOOLS, 'event_store_cross_build.py'):
        (root / 'tools' / name).write_bytes(Path(lab.__file__).with_name(name).read_bytes())
    subprocess.run(['git', 'init', '-q', str(root)], check=True)
    subprocess.run(['git', '-C', str(root), '-c', 'user.name=fixture', '-c', 'user.email=fixture@invalid', 'commit', '--allow-empty', '-qm', 'fixture'], check=True)
    plan = cross.plan(root)
    assert {n for n in plan['files'] if n.startswith('config/')} == set(cross.CONFIG_EXAMPLES)
    assert all(plan['files'][n]['inputKind'] == 'redacted-test-fixture' for n in cross.CONFIG_EXAMPLES)
    lab.save(out / 'plan.json', plan)
    frozen = cross.capture(root, out / 'plan.json', lab.sha(out / 'plan.json'), out / 'snapshot', True)
    build_dir = lab.fresh(out / 'synthetic-build')
    binaries = lab.fresh(build_dir / 'binaries')
    elf = bytearray(64)
    elf[:6] = b'\x7fELF\x02\x01'
    struct.pack_into('<H', elf, 18, 183)
    arheader = ('fixture.o/'.ljust(16) + '0'.ljust(12) + '0'.ljust(6) + '0'.ljust(6) + '100644'.ljust(8) + str(len(elf)).ljust(10) + '`\n').encode('ascii')
    artifacts = {}
    for meta in plan['targets'].values():
        p = binaries / meta['artifact']
        p.write_bytes(b'!<arch>\n' + arheader + elf if meta['kind'] == 'static' else elf)
        artifacts[p.name] = dict(sha256=lab.sha(p), kind=meta['kind'], architecture='AArch64')
    lab.save(build_dir / 'build-result.json', dict(success=True, synthetic=True, manifestSha256=frozen['manifestSha256'], artifacts=artifacts))
    (out / 'private-sqlite.so').write_bytes(elf)
    request = types.SimpleNamespace(snapshot=out / 'snapshot', manifest_sha256=frozen['manifestSha256'], build_output=build_dir,
            build_result_sha256=lab.sha(build_dir / 'build-result.json'), sqlite_library=out / 'private-sqlite.so',
            sqlite_sha256=lab.sha(out / 'private-sqlite.so'), purpose='tool-selftest', suite='all', output=out / 'bundle',
            duration=8, rate=20, rpc_timeout=30, profile=['delete-full', 'wal-full'], collect_databases=True)
    result = lab.prepare(request)
    checksum_bytes = (request.output / 'inputs' / 'SHA256SUMS').read_bytes()
    assert b'\r' not in checksum_bytes and checksum_bytes.endswith(b'\n'), 'checksum manifest must use LF on every host'
    contract = lab.verify_bundle(request.output, result['bundleSha256'])
    lab.verify_inputs(request.output / 'inputs')
    assert len(contract['request']['runs']) == 3
    matrix = lab.read(request.output / 'inputs/matrix.json')
    assert matrix['cases'][0]['argv'][-1] == '@LAB@/libsqlite3.so'
    assert len(matrix['cases'][2]['argv']) == 3 and matrix['accepted24h'] is False
    assert len(matrix['cases']) == 4, 'TARGET_FILE angle brackets must preserve all four CTest entries'
    assert matrix['usedByAdditionalMatrix'] == [lab.MIGRATION_PROBE, lab.CAPACITY_BINARY]
    assert matrix['additionalMatrix'][1] == lab.CAPACITY_CASE
    original_bounded = lab.bounded
    def fake_capacity(argv, log, timeout, cwd, env):
        assert argv == [str(request.output / 'inputs' / lab.CAPACITY_BINARY), '--enospc']
        assert timeout == 180 and cwd == request.output / 'inputs'
        assert env['SQLITE_LIBRARY'] == str(cwd / 'libsqlite3.so') and env['LD_LIBRARY_PATH'] == str(cwd)
        log.write_text('PASS real-ENOSPC-delete-recovery\nPASS real-ENOSPC-wal-recovery\n')
        return 0
    try:
        lab.bounded = fake_capacity
        assert lab.run_capacity_matrix(request.output / 'inputs', out, {})['passed']
        def missing_marker(argv, log, timeout, cwd, env):
            log.write_text('PASS real-ENOSPC-delete-recovery\n')
            return 0
        lab.bounded = missing_marker
        assert not lab.run_capacity_matrix(request.output / 'inputs', out, {})['passed']
    finally:
        lab.bounded = original_bounded
    passed('ENOSPC dispatch contract and both recovery markers required (synthetic)')
    assert matrix['additionalMatrix'][0]['minimumTestsByModule'] == lab.PYTHON_MATRIX_MINIMA
    for flat, original in lab.CONFIG_EXAMPLES.items():
        fixture = lab.read(request.output / 'inputs' / flat)
        assert fixture['username'] == fixture['password'] == '' and fixture['mqttForward'] == {'enabled': False, 'broker': ''}
        meta = contract['request']['configFixtures'][flat]
        assert meta['sourceSha256'] == lab.sha(root / original) and meta['sha256'] == lab.sha(request.output / 'inputs' / flat)
        assert meta['onlyCredentialFieldsChanged']
    passed('three explicit config fixtures redacted and source/fixture hashes distinguished; unrelated config excluded')
    rejects('unredacted credential guard', lambda: cross.validate_config_example(b'{"password":"synthetic-secret"}'))
    passed('synthetic frozen bundle, CTest fixture arguments, all profiles and exact hashes')
    rejects('bundle SHA gate', lambda: lab.verify_bundle(request.output, '0' * 64))
    (request.output / 'inputs/libsqlite3.so').write_bytes(b'tamper')
    rejects('private library content tamper', lambda: lab.verify_inputs(request.output / 'inputs'))
    with (request.output / 'bundle.tar.gz').open('ab') as stream:
        stream.write(b'tamper')
    rejects('bundle archive tamper', lambda: lab.verify_bundle(request.output, result['bundleSha256']))
    before = dict(bootId='synthetic', services={s: {'MainPID': '10', 'startTicks': '100'} for s in lab.SERVICES}, processes={})
    lab.same_production(before, copy.deepcopy(before))
    for field in ('MainPID', 'startTicks'):
        after = copy.deepcopy(before)
        after['services'][lab.SERVICES[0]][field] = 'changed'
        rejects('production ' + field + ' drift', lambda: lab.same_production(before, after))
    rejects('production empty roster', lambda: lab.same_production(dict(services={}), dict(services={})))
    remote_fixture = lab.fresh(out / 'collection-fixture')
    data_dir = lab.fresh(remote_fixture / 'run-fixture/result')
    (data_dir / 'report.md').write_text('# 合成回传夹具\n', encoding='utf-8')
    source = data_dir / 'sample.db'
    with sqlite3.connect(str(source)) as writer:
        writer.execute('PRAGMA journal_mode=WAL')
        writer.execute('PRAGMA wal_autocheckpoint=0')
        writer.execute('CREATE TABLE sample(id INTEGER)')
        writer.executemany('INSERT INTO sample VALUES(?)', [(i,) for i in range(40)])
        writer.commit()
        hashes = (lab.sha(source), lab.sha(Path(str(source) + '-wal')))
        lab.collect_local(remote_fixture, True)
        assert hashes == (lab.sha(source), lab.sha(Path(str(source) + '-wal')))
    returned = lab.fresh(out / 'download-fixture')
    lab.unpack_return(remote_fixture / 'return.tar.gz', remote_fixture / 'return.json', returned)
    with sqlite3.connect(str(returned / 'run-fixture/result/sample.db')) as db:
        assert db.execute('SELECT count(*) FROM sample').fetchone()[0] == 40
    assert not any(n.endswith(('-wal', '-shm')) for n in lab.read(remote_fixture / 'return.json')['files'])
    passed('read-only WAL sample backup, source hashes unchanged and exact return archive')
    return_manifest = lab.read(remote_fixture / 'return.json')
    return_manifest['files']['run-fixture/result/sample.db'] = '0' * 64
    lab.save(out / 'bad-return.json', return_manifest)
    rejects('return content SHA gate', lambda: lab.unpack_return(remote_fixture / 'return.tar.gz', out / 'bad-return.json', out / 'bad-return'))
    assert not lab.safe('../escape') and not lab.safe('/absolute')
    passed('return traversal gate')
    lab.detach_allowed({'runs': [{'suite': 'soak', 'purpose': 'soak', 'duration': 86400}]})
    for run in ({'suite': 'soak', 'purpose': 'soak', 'duration': 8},
                {'suite': 'soak', 'purpose': 'tool-selftest', 'duration': 90000},
                {'suite': 'matrix', 'purpose': 'tool-selftest', 'duration': 90000}):
        rejects('detach short/matrix purpose gate', lambda: lab.detach_allowed({'runs': [run]}))
    detached = lab.fresh(out / 'detached-fixture')
    (detached / 'request.json').write_text('{}', encoding='utf-8')
    (detached / 'event_store_soak_22_16.py').write_bytes(Path(lab.__file__).read_bytes())
    parent_code = '''import json,os,pathlib,sys
sys.path.insert(0,sys.argv[1])
import event_store_soak_22_16 as lab
p=pathlib.Path(sys.argv[2])
worker="import pathlib,sys,time; time.sleep(2); pathlib.Path(sys.argv[1]).write_text('survived parent exit')"
child,receipt=lab.launch_session([sys.executable,'-c',worker,str(p/'done.txt')],p,p/'worker.log')
receipt.update(requestSha256=lab.sha(p/'request.json'),runnerSha256=lab.sha(p/'event_store_soak_22_16.py'))
lab.save(p/'host-launch.json',receipt)
os._exit(0)
'''
    subprocess.run([sys.executable, '-c', parent_code, str(Path(lab.__file__).parent), str(detached)], check=True)
    first = lab.status_at(detached)
    assert first['state'] == 'running' and first['passed'] is None and first['accepted24h'] is False
    receipt = lab.read(detached / 'host-launch.json')
    # Preserve the genuine receipt; test PID reuse against a separate evidence directory.
    stale = lab.fresh(out / 'stale-receipt')
    for name in ('request.json', 'event_store_soak_22_16.py'):
        (stale / name).write_bytes((detached / name).read_bytes())
    lab.save(stale / 'host-launch.json', dict(receipt, startTicks='0'))
    assert lab.status_at(stale)['state'] == 'failed'
    passed('status rejects reused PID/startTicks')
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline and not (detached / 'done.txt').exists():
        time.sleep(.1)
    assert (detached / 'done.txt').read_text() == 'survived parent exit'
    passed('new session survives parent exit; running has no pass/24h verdict')
    lab.save(detached / 'host-result.json', {'passed': True, 'productionUnchanged': True})
    lab.collect_local(detached, False)
    final = lab.status_at(detached)
    assert final['state'] == 'completed' and final['collectionReady'] and final['accepted24h'] is False
    passed('completed status requires finished return bundle')
    native_lab = lab.fresh(out / 'native-python-lab')
    native_result = lab.fresh(out / 'native-python-result')
    for name in lab.MIGRATION_TOOLS:
        (native_lab / name).write_bytes(Path(lab.__file__).with_name(name).read_bytes())
    (native_lab / lab.MIGRATION_SOURCE).write_bytes(Path(lab.__file__).parents[1].joinpath('src', lab.MIGRATION_SOURCE).read_bytes())
    probe = args.migration_probe.resolve(strict=True)
    (native_lab / lab.MIGRATION_PROBE).write_bytes(probe.read_bytes())
    (native_lab / lab.MIGRATION_PROBE).chmod(0o755)
    python_result = lab.run_python_matrix(native_lab, native_result, os.environ)
    lab.save(native_result / 'matrix-item.json', python_result)
    assert python_result['passed'] and python_result['unittest']['testsRun'] >= 47 and not python_result['unittest']['skipped'], 'native Python matrix failed; inspect native-python-result/*.json and *.log'
    assert python_result['unittest']['probeTestPassed']
    passed('real native migration/history 46 plus query 1; C++ probe used, zero skips')
    (native_lab / lab.MIGRATION_PROBE).rename(native_lab / 'probe.saved')
    rejects('missing migration probe never skips', lambda: lab.run_python_matrix(native_lab, out / 'must-not-run', os.environ))
    for flat, original in lab.CONFIG_EXAMPLES.items():
        data = Path(lab.__file__).parents[1].joinpath(original).read_bytes()
        redacted, metadata = cross.config_fixture(data)
        (native_lab / flat).write_bytes(redacted)
        lab.save(native_lab / (flat + '.provenance.json'), metadata)
    config_workspace = lab.fresh(out / 'native-config-cwd')
    lab.materialize_config_examples(native_lab, config_workspace)
    loader = args.config_loader.resolve(strict=True)
    loader_copy = native_lab / 'config_loader_test'
    loader_copy.write_bytes(loader.read_bytes())
    loader_copy.chmod(0o755)
    code = lab.bounded([str(loader_copy)], out / 'native-config-loader.log', 180, config_workspace)
    assert code == 0, 'native config_loader_test failed; inspect native-config-loader.log'
    passed('real config_loader_test reads only private three-example redacted cwd')
    for command, arguments in (('execute', ['--bundle', str(request.output), '--bundle-sha256', result['bundleSha256']]),
                               ('collect', ['--remote-lab', '/not-private'])):
        completed = subprocess.run([sys.executable, lab.__file__, command, *arguments, '--output', str(out / ('forbidden-' + command))], capture_output=True, text=True)
        assert completed.returncode == 2 and 'requires --execute' in completed.stderr
        (out / (command + '-gate.log')).write_text(completed.stderr, encoding='utf-8')
        passed(command + ' explicit execution gate')
    result = dict(passed=True, accepted24h=False, syntheticBundleFixtures=True, realSourceFrozen=False, remoteConnections=0,
                  realMigrationProbeSha256=lab.sha(probe), realConfigLoaderSha256=lab.sha(loader), cases=cases)
    lab.human_report(out, result, [(c['name'], '通过', '仅本地夹具', '项') for c in cases],
                     ['没有冻结真实仓库，没有连接设备，没有执行 SSH/namespace/systemctl。', '合成 ELF 只有架构头；另行运行真实 native probe 和 config_loader_test。合成 bundle 已故意篡改，不能用于设备。'])
    print(json.dumps(result, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
