"""Private manager observation endpoint, NOT the product installer/boot worker."""
import importlib.util
import json
import os
from pathlib import Path
import resource
import sys
import time
from unittest import mock

ROOT = Path('/tmp/gw-nativeboot-082071e-23-C1')


def need(value, message):
    if not value:
        raise ValueError(message)


def private(path):
    path = Path(path)
    need(path.is_absolute() and path.resolve() == path and ROOT in path.parents,
         'observation endpoint escaped private root')
    return path


def publish(path, value):
    with private(path).open('x') as output:
        json.dump(value, output, sort_keys=True)
        output.write('\n'); output.flush(); os.fsync(output.fileno())


def wait(path):
    end = time.monotonic() + 25
    while not private(path).exists():
        need(time.monotonic() < end, 'private observation deadline')
        time.sleep(0.02)


def main():
    for key, value in ((resource.RLIMIT_CPU, 15), (resource.RLIMIT_AS, 256 * 1024**2),
                       (resource.RLIMIT_FSIZE, 32 * 1024**2), (resource.RLIMIT_NOFILE, 64),
                       (resource.RLIMIT_CORE, 0)):
        resource.setrlimit(key, (value, value))
    action = sys.argv[1]
    if action == 'ota-owner':
        staging = private(sys.argv[2])
        spec = importlib.util.spec_from_file_location('c1_pinned_handoff', Path(__file__).with_name('ota-health-result.py'))
        health = importlib.util.module_from_spec(spec); spec.loader.exec_module(health)
        protocol = health.RuntimeHandoff(staging, sys.argv[3], sys.argv[4])
        spec = importlib.util.spec_from_file_location('c1_shared_command_recorder', ROOT / 'source/manager-child.py')
        records = importlib.util.module_from_spec(spec); spec.loader.exec_module(records)
        recorder = records.ManagerChecks.__new__(records.ManagerChecks)
        recorder.logs = ROOT / 'manager-commands'; recorder.commands = []
        recorder.units = records.unit_names(); recorder.unit_starts = {}
        recorder.original_run = health.subprocess.run
        def observe(owner, record):
            need(record['phase'] == 'OWNER_OWNED' and not record['runtimeMayChange'], 'not observation-only ownership')
            publish(staging / 'owned-observed.json', dict(pid=os.getpid(), at=time.monotonic(), record=record))
            wait(staging / 'owner-return')
        with mock.patch.object(health.subprocess, 'run', recorder.run):
            protocol.own(install=observe)  # Callback/logging adapters only; no product install/global lock.
        return 0
    home = private(sys.argv[2])
    if action == 'gate':
        publish(home / 'gate-enter.json', dict(pid=os.getpid(), at=time.monotonic()))
        wait(home / 'gate-release')
        publish(home / 'gate-exit.json', dict(pid=os.getpid(), at=time.monotonic()))
        return 1 if home.name == 'gate-fail' else 0
    if action == 'pre-reader':
        need((home / 'gate-exit.json').exists(), 'reader before gate exit')
        publish(home / 'pre-reader.json', dict(pid=os.getpid(), at=time.monotonic()))
        return 0
    if action in ('reader', 'production-boot-finish'):
        need((home / 'gate-exit.json').exists(), 'observer before gate exit')
        publish(home / ('reader.json' if action == 'reader' else 'worker.json'),
                dict(pid=os.getpid(), at=time.monotonic()))
        return 0
    raise ValueError('unapproved private entry action')


if __name__ == '__main__':
    raise SystemExit(main())
