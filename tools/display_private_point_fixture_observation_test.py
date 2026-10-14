"""The real fixture subprocess, independently read by the public native store API."""
import json
import os
from pathlib import Path
import resource
import signal
import stat
import subprocess
import sys
import time


def save(path, value):
    with path.open('x', encoding='ascii') as stream:
        json.dump(value, stream, sort_keys=True, indent=2)
        stream.write('\n')


def main():
    if len(sys.argv) != 4 or os.geteuid() == 0:
        raise ValueError('NONROOT_EXACT_HELPER_READER_CAPTURE_REQUIRED')
    helper, reader = (str(Path(value).resolve(strict=True)) for value in sys.argv[1:3])
    out = Path(sys.argv[3])
    out.mkdir(mode=0o700)
    assert resource.getrlimit(resource.RLIMIT_AS)[0] <= 512*1024*1024
    assert resource.getrlimit(resource.RLIMIT_FSIZE) == (16*1024*1024, 16*1024*1024)
    shm = Path('/dev/shm/d24_display_20261011_01_test')
    assert not os.path.lexists(shm), 'not fresh; no cleanup/retry'
    command = ['/usr/bin/timeout', '--signal=TERM', '--kill-after=2s', '15s', helper,
               '--run', '--name', shm.name, '--phase-seconds', '3']
    save(out/'command.json', command)
    observations = []
    process = None
    completed = False
    primary = None
    cleanup = []
    began = time.monotonic()

    def phases():
        assert (out/'stdout.raw').stat().st_size <= 65536
        assert (out/'stderr.raw').stat().st_size <= 8192
        data = (out/'stdout.raw').read_bytes()
        return [json.loads(line) for line in data.splitlines() if line.endswith(b'}')]

    def wait_phase(name):
        while time.monotonic()-began < 20:
            found = [item for item in phases() if item.get('phase') == name]
            if found:
                assert len(found) == 1
                return found[0]
            assert process.poll() is None, (name, 'fixture ended before phase')
            time.sleep(.02)
        raise TimeoutError('fixture phase deadline')

    def read(name):
        folder = out/name
        folder.mkdir()
        argv = ['/usr/bin/timeout', '--signal=TERM', '--kill-after=1s', '4s', reader, '--read-private']
        save(folder/'command.json', argv)
        with (folder/'stdout.raw').open('xb') as stdout, (folder/'stderr.raw').open('xb') as stderr:
            result = subprocess.run(argv, stdin=subprocess.DEVNULL, stdout=stdout, stderr=stderr, timeout=6, check=False)
        save(folder/'result.json', dict(actualExit=result.returncode))
        assert result.returncode == 0 and (folder/'stderr.raw').stat().st_size == 0
        value = json.loads((folder/'stdout.raw').read_bytes())
        assert value['device'] == ready['device'] and value['inode'] == ready['inode']
        assert [point['index'] for point in value['points']] == [880001, 880002]
        observations.append(dict(phase=name, nativePublicApi=value, actualReaderExit=result.returncode))
        return value['points']

    with (out/'stdout.raw').open('xb') as stdout, (out/'stderr.raw').open('xb') as stderr:
        try:
            process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=stdout, stderr=stderr,
                                       start_new_session=True)
            save(out/'process.json', dict(pid=process.pid, session=process.pid, uid=os.geteuid()))
            ready = wait_phase('READY')
            assert ready['version'] == 11 and ready['layoutBytes'] <= 16*1024*1024
            assert ready['refreshMs'] == 200 and ready['ttlMs'] == 1000 and ready['uid'] == os.geteuid()
            assert ready['mode'] == 0o600
            for phase, numbers, quality in (('A', [12.5, 1.0], 1), ('B', [37.75, 2.0], 1), ('BAD', [37.75, 2.0], 0)):
                declared = wait_phase(phase)
                assert declared['machineCode'] == 'COMM202600998' and declared['meterCode'] == 'DISPLAY_PROBE'
                assert declared['temperatureIndex'] == 880001 and declared['counterIndex'] == 880002
                points = read(phase)
                assert [p['value'] for p in points] == numbers
                assert all(p['quality'] == quality and not p['stale'] and p['expireAt']-p['ts'] == 1000 for p in points)
            stale = wait_phase('STALE_WAIT')
            fresh = read('STALE_FRESH')
            assert all(p['quality'] == 1 and not p['stale'] and p['ts'] == stale['lastGoodTs'] for p in fresh)
            time.sleep(1.3)
            old = read('STALE_EXPIRED')
            assert [p['value'] for p in old] == [37.75, 2.0]
            assert all(p['quality'] == 1 and p['stale'] and p['expireAt'] == stale['expireAt'] and
                       p['ts'] == stale['lastGoodTs'] for p in old)
            wait_phase('DONE_RETAINED')
            process.wait(timeout=max(.1, 22-(time.monotonic()-began)))
            assert process.returncode == 0 and (out/'stderr.raw').stat().st_size == 0
            current = os.lstat(shm)
            assert stat.S_ISREG(current.st_mode) and current.st_uid == os.geteuid()
            assert (current.st_dev, current.st_ino) == (ready['device'], ready['inode'])
            assert current.st_size == ready['layoutBytes']
            # The helper deliberately retained its object; only this successful scope-owned test removes it.
            os.unlink(shm)
            completed = True
        except BaseException as failure:
            primary = type(failure).__name__
            raise
        finally:
            if process is not None and process.poll() is None:
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait(timeout=2)
                except Exception as failure:
                    cleanup.append(type(failure).__name__)
            save(out/'result.json', dict(status='REAL_FIXTURE_PUBLIC_API_OBSERVATION_PASS' if completed else 'FAILED_RETAINED',
                 primaryError=primary, cleanupErrors=cleanup, actualExit=None if process is None else process.returncode,
                 observations=observations, successOwnedObjectRemoved=completed, failedObjectNotUnlinked=not completed,
                 fsizeBytes=resource.getrlimit(resource.RLIMIT_FSIZE)[0], uid=os.geteuid(), elapsedSeconds=time.monotonic()-began,
                 armOrDisplayAcceptance=False))
    print('PASS real producer A/B/bad-quality/actual-expiry independently observed via native public API')


if __name__ == '__main__':
    main()
