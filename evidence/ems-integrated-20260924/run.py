"""Run pinned native integration targets in private test namespaces, not on devices."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import tempfile
import time


HERE = Path(__file__).resolve().parent
SOURCE = HERE.parents[1]
BUILD = Path('/var/tmp/ems-production-candidate-20260924-build')


def git(*args):
    executable, worktree = 'git', str(SOURCE)
    pointer = SOURCE / '.git'
    if pointer.is_file() and re.match(r'gitdir: [A-Za-z]:', pointer.read_text()):
        executable = '/mnt/c/Program Files/Git/cmd/git.exe'
        worktree = subprocess.check_output(['wslpath', '-w', str(SOURCE)], text=True).strip()
    return subprocess.check_output([executable, '-C', worktree, *args], text=True).strip()


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('stage')
    parser.add_argument('--targets', nargs='+', required=True)
    parser.add_argument('--tests', required=True)
    args = parser.parse_args()
    if not re.fullmatch(r'[a-z0-9-]+', args.stage):
        parser.error('restricted stage name required')
    if git('status', '--porcelain', '--untracked-files=no'):
        parser.error('commit all tracked integration changes before testing')
    output = HERE / args.stage
    output.mkdir(exist_ok=False)
    result = {'sourceCommit': git('rev-parse', 'HEAD'), 'targets': args.targets,
              'nativeOnly': True, 'deviceAccess': False, 'steps': {},
              'compiler': subprocess.check_output(['c++', '--version'], text=True),
              'runnerSha256': digest(Path(__file__)),
              'isolationSha256': digest(HERE / 'isolated.sh')}
    BUILD.mkdir(exist_ok=True)

    def run(name, command, limit, env=None):
        start = time.monotonic()
        with (output / (name + '.log')).open('wb') as log:
            process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT,
                                       env=env, start_new_session=True)
            timed_out = False
            try:
                code = process.wait(timeout=limit)
            except subprocess.TimeoutExpired:
                timed_out = True
                os.killpg(process.pid, signal.SIGKILL)
                code = process.wait()
        result['steps'][name] = {'argv': command, 'exitCode': code,
                                 'timeout': timed_out,
                                 'seconds': round(time.monotonic() - start, 3)}
        print(name, code, flush=True)
        if code or timed_out:
            raise RuntimeError(name + ' failed; see retained log')

    try:
        run('configure', ['cmake', '-S', str(SOURCE), '-B', str(BUILD),
                         '-DCMAKE_BUILD_TYPE=Debug', '-DBUILD_TESTING=ON'], 120)
        run('build', ['cmake', '--build', str(BUILD), '--target', *args.targets,
                     '--parallel', '2'], 900)
        result['binaries'] = {name: digest(BUILD / name) for name in args.targets
                              if (BUILD / name).is_file()}
        with tempfile.TemporaryDirectory(prefix='test-', dir=BUILD) as temporary:
            env = dict(os.environ, GATEWAY_MIGRATION_TEST_BACKUP_DIR=temporary)
            run('ctest', ['unshare', '--mount', '--net', '--ipc', '--pid', '--fork',
                          '--mount-proc', 'sh', str(HERE / 'isolated.sh'), 'ctest',
                          '--test-dir', str(BUILD), '--output-on-failure',
                          '--no-tests=error', '--timeout', '60', '-j1', '-R', args.tests],
                600, env)
        result['sourceUnchanged'] = not git('status', '--porcelain', '--untracked-files=no')
        if not result['sourceUnchanged']:
            raise RuntimeError('tracked integration source changed during testing')
        result['status'] = 'PASS_NATIVE_INTEGRATION_ONLY'
        return 0
    except Exception as error:
        result['status'] = 'FAIL'
        result['error'] = str(error)
        return 1
    finally:
        result['logs'] = {p.name: digest(p) for p in output.glob('*.log')}
        (output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')


if __name__ == '__main__':
    raise SystemExit(main())
