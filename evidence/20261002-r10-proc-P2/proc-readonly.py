"""Fixed B1 namespace/interface inventory. Read-only; no signal or mutation."""
import errno
import hashlib
import json
import os
from pathlib import Path
import platform
import stat
import threading
import time

ROOT = Path('/tmp/gw-r10-68de70f-23-B1')
BOOT = '87ba3880-1293-4b45-8826-8386e8ffb40f'
TARGETS = {'pid': 'pid:[4026532696]', 'mnt': 'mnt:[4026532695]', 'net': 'net:[4026532698]'}
RECEIPT_SHA = 'acf7d4d7d85a1e61563cb4b094e952ef701a3e0f5760e30044c38d0718d25d2f'
KEYS = ('machineCode', 'bootId', 'stateDir', 'stateSha256', 'approvalSha256', 'activeSha256',
        'guardSha256', 'configSha256', 'markerSha256', 'readers', 'runtimeProcesses', 'segments',
        'offlineLock', 'segmentIdentities', 'readerStartTicks', 'readerShmMaps')


def read_interface(path):
    try:
        with Path(path).open('rb') as stream: raw = stream.read(65537)
        assert len(raw) <= 65536
        return dict(path=path, readable=True, errno=None, raw=raw.decode('ascii', 'strict'))
    except OSError as error:
        return dict(path=path, readable=False, errno=error.errno, errorType=type(error).__name__, message=str(error))


def bounded_entries(directory, cap, numeric=False):
    rows = []
    for path in directory.iterdir():
        if numeric and not path.name.isdigit(): continue
        assert len(rows) < cap, 'directory inventory bound'
        rows.append(path)
    return rows


def inventory():
    processes = bounded_entries(Path('/proc'), 4096, numeric=True)
    started = time.monotonic(); holders = []; errors = []; gone = []; descriptors = 0; closed_fds = 0
    def uncertainty(path, error):
        assert len(errors) < 32
        errors.append(dict(path=str(path), errno=error.errno, message=str(error)))
    for process in processes:
        assert time.monotonic()-started <= 10, 'proc inventory deadline'
        try:
            status = (process/'stat').read_text(); assert len(status) <= 4096
            fields = status.rsplit(')', 1)[1].split(); assert len(fields) >= 20
            identity = dict(pid=int(process.name), startTicks=fields[19], state=fields[0])
            for role in ('pid', 'pid_for_children', 'mnt', 'net'):
                path = process/'ns'/role
                try: link = os.readlink(str(path))
                except OSError as error:
                    # Older kernels may omit pid_for_children; do not invent support.
                    if role == 'pid_for_children' and error.errno == errno.ENOENT: continue
                    uncertainty(path, error); continue
                if link in TARGETS.values(): holders.append(dict(identity, kind='membership', role=role, namespace=link))
            try: fds = bounded_entries(process/'fd', 512)
            except OSError as error:
                uncertainty(process/'fd', error); continue
            descriptors += len(fds); assert descriptors <= 16384
            for fd in fds:
                try: link = os.readlink(str(fd))
                except OSError as error:
                    if error.errno == errno.ENOENT:
                        closed_fds += 1  # Observed FD already closed, including scandir's own FD.
                        continue
                    uncertainty(fd, error); continue
                if link in TARGETS.values(): holders.append(dict(identity, kind='open-namespace-fd', fd=fd.name, namespace=link))
            assert len(holders) <= 128
        except (FileNotFoundError, ProcessLookupError):
            assert len(gone) < 64; gone.append(int(process.name))
    return dict(scannedProcesses=len(processes), scannedDescriptors=descriptors, holders=holders,
                alreadyClosedDescriptors=closed_fds,
                readErrors=errors, disappearingProcesses=gone, elapsedSeconds=time.monotonic()-started,
                complete=not errors and not gone, atomicSnapshot=False,
                proofScope='current membership and open namespace FD observation, not kernel refcount certification')


def root_evidence():
    assert ROOT.resolve() == ROOT
    rows = []
    for directory in (ROOT, ROOT/'source', ROOT/'bin', ROOT/'work'):
        info = directory.lstat()
        assert stat.S_ISDIR(info.st_mode) and not directory.is_symlink() and info.st_uid == 0
        entries = bounded_entries(directory, 16)
        contents = []
        for p in sorted(entries):
            s = p.lstat()
            contents.append(dict(name=p.name, mode=stat.S_IMODE(s.st_mode), uid=s.st_uid,
                                 dev=s.st_dev, ino=s.st_ino, bytes=s.st_size, symlink=p.is_symlink()))
        rows.append(dict(path=str(directory), dev=info.st_dev, ino=info.st_ino, mode=stat.S_IMODE(info.st_mode), contents=contents))
    receipt = ROOT/'result.json'; info = receipt.lstat()
    assert stat.S_ISREG(info.st_mode) and not receipt.is_symlink() and info.st_uid == 0 and info.st_nlink == 1
    assert info.st_size <= 131072
    with receipt.open('rb') as stream: raw = stream.read(131073)
    assert len(raw) == info.st_size and len(raw) <= 131072
    digest = hashlib.sha256(raw).hexdigest()
    assert digest == RECEIPT_SHA, 'B1 receipt drift'
    value = json.loads(raw)
    return dict(directories=rows, receiptBytes=len(raw), receiptSha256=digest,
                receiptSummary={k:value.get(k) for k in ('primaryError','cleanupError','baselineChanges','allInputHashesUnchanged')})


def probe(baseline):
    result = dict(readOnly=True, remoteWrites=0, signalsSent=0, namespaceCreated=False, targets=TARGETS)
    try:
        assert os.geteuid() == 0 and platform.machine() == 'aarch64'
        assert Path('/proc/sys/kernel/random/boot_id').read_text().strip() == BOOT
        assert threading.current_thread() is threading.main_thread()
        tid = threading.get_native_id(); assert tid == os.getpid()
        result['kernelRelease'] = platform.release()
        with Path('/proc/version').open('rb') as stream: version = stream.read(4097)
        assert len(version) <= 4096; result['kernelVersionRaw'] = version.decode()
        result['interfaces'] = [read_interface('/proc/self/task/%d/children' % tid), read_interface('/proc/thread-self/children')]
        result['before'] = baseline(); assert result['before']['snapshot']['machineCode'] == 'COMM202600999'
        result['rootBefore'] = root_evidence(); result['inventory'] = inventory()
    except BaseException as error:
        result['primaryError'] = dict(type=type(error).__name__, message=str(error))
    finally:
        try:
            result['after'] = baseline(); result['rootAfter'] = root_evidence()
            assert 'before' in result
            result['baselineChanges'] = [k for k in KEYS if result['before']['snapshot'][k] != result['after']['snapshot'][k]]
            if result['before']['parentNamespace'] != result['after']['parentNamespace']: result['baselineChanges'].append('parentNamespace')
            assert result.get('rootBefore') == result['rootAfter'], 'B1 root/receipt drift'
        except BaseException as error: result['postcheckError'] = dict(type=type(error).__name__, message=str(error))
    observed = result.get('inventory', {})
    result['outcome'] = ('UNKNOWN' if any(k in result for k in ('primaryError','postcheckError')) or not observed.get('complete')
                         or result.get('baselineChanges') != [] else 'HOLDERS_OBSERVED' if observed['holders'] else 'NO_REFERENCES_OBSERVED')
    return result
