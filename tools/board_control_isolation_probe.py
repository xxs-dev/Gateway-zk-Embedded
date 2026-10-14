#!/usr/bin/env python3
"""Read-only proof for a systemd-isolated board test process, not a deployment."""
import argparse
import errno
import hashlib
import json
import os
from pathlib import Path
import re
import socket
import stat
import subprocess


def require(condition, message):
    if not condition:
        raise ValueError(message)


def check_namespace(current, host, shared, label):
    pattern = r'[a-z]+:\[[0-9]+\]'
    require(all(isinstance(value, str) and re.fullmatch(pattern, value)
                for value in (current, host, shared)), 'invalid ' + label + ' namespace proof')
    require(current != host, 'service uses host ' + label + ' namespace')
    require(current == shared, 'services do not share isolated ' + label + ' namespace')


def mount_rows(text):
    rows = []
    for line in text.splitlines():
        fields = line.split()
        separator = fields.index('-')
        unescape = lambda value: re.sub(r'\\([0-7]{3})',
                                        lambda match: chr(int(match.group(1), 8)), value)
        rows.append({'path': unescape(fields[4]), 'options': fields[5].split(','),
                     'type': fields[separator + 1]})
    return rows


def check_links(links, routes):
    require(isinstance(links, list) and links, 'network inventory missing')
    require(any(row.get('ifname') == 'lo' for row in links), 'loopback missing')
    require(all(row.get('ifname') == 'lo' or
                (row.get('ifname') == 'sit0' and 'UP' not in row.get('flags', []))
                for row in links), 'physical or unexpected network interface visible')
    # The local table may contain only loopback routes; inspect all tables,
    # not just the main IPv4 table, to avoid missing policy/IPv6 egress.
    require(all(row.get('dev') == 'lo' for row in routes), 'non-loopback route visible')


def check_status(text):
    fields = dict(line.split(':', 1) for line in text.splitlines() if ':' in line)
    for name in ('CapInh', 'CapPrm', 'CapEff', 'CapBnd', 'CapAmb'):
        require(int(fields[name].strip(), 16) == 0, 'capabilities remain: ' + name)
    require(fields['NoNewPrivs'].strip() == '1', 'privilege escalation not blocked')
    require(fields['Seccomp'].strip() == '2', 'seccomp filter not active')
    return {key: fields[key].strip() for key in
            ('CapInh', 'CapPrm', 'CapEff', 'CapBnd', 'CapAmb', 'NoNewPrivs', 'Seccomp')}


def check_mounts(rows):
    system = [row for row in rows if row['path'] == '/sys' or row['path'].startswith('/sys/')]
    require(system and any(row['path'] == '/sys' for row in system), 'sysfs inventory missing')
    require(all('ro' in row['options'] for row in system), 'writable sysfs/control group mount')
    require(any(row['path'] == '/run' and row['type'] == 'tmpfs' for row in rows),
            'host /run is not hidden behind private tmpfs')
    require(any(row['path'] == '/dev' and row['type'] == 'tmpfs' for row in rows),
            'private device mount missing')


def device_inventory(root=Path('/dev')):
    allowed = {(1, 3), (1, 5), (1, 7), (1, 8), (1, 9), (5, 0), (5, 2)}
    rows = []
    for path in root.rglob('*'):
        value = path.lstat()
        if stat.S_ISBLK(value.st_mode):
            raise ValueError('physical block device visible: ' + str(path))
        if stat.S_ISCHR(value.st_mode):
            identity = (os.major(value.st_rdev), os.minor(value.st_rdev))
            require(identity in allowed, 'unexpected character device: ' + str(path))
            rows.append({'path': str(path), 'major': identity[0], 'minor': identity[1]})
    require(rows, 'device inventory empty')
    return sorted(rows, key=lambda row: row['path'])


def blocked_socket(family, kind, protocol=0):
    try:
        opened = socket.socket(family, kind, protocol)
    except OSError as error:
        require(error.errno in (errno.EPERM, errno.EACCES, errno.EAFNOSUPPORT,
                                errno.EPROTONOSUPPORT), 'unexpected socket error: ' + str(error))
        return error.errno
    opened.close()
    raise ValueError('forbidden socket family/type remains available')


def blocked_host_root():
    try:
        os.stat('/proc/1/root/run/systemd')
    except OSError as error:
        require(error.errno in (errno.EPERM, errno.EACCES),
                'host root denial not proved: ' + str(error))
        return error.errno
    raise ValueError('host root accessible through procfs')


def probe(args):
    current = os.readlink('/proc/self/ns/net')
    check_namespace(current, args.host_netns, args.shared_netns, 'network')
    current_ipc = os.readlink('/proc/self/ns/ipc')
    check_namespace(current_ipc, args.host_ipcns, args.shared_ipcns, 'IPC')
    require(len(Path('/proc/sysvipc/shm').read_text().splitlines()) == 1,
            'unexpected existing System V shared memory')
    command = lambda argv: json.loads(subprocess.check_output(argv, universal_newlines=True,
                                                              timeout=10))
    links = command(['ip', '-j', 'link', 'show'])
    routes = command(['ip', '-j', '-4', 'route', 'show', 'table', 'all'])
    routes += command(['ip', '-j', '-6', 'route', 'show', 'table', 'all'])
    check_links(links, routes)
    status = check_status(Path('/proc/self/status').read_text())
    check_mounts(mount_rows(Path('/proc/self/mountinfo').read_text()))
    for path in ('/run/systemd/private', '/run/dbus/system_bus_socket',
                 '/dev/ttyS0', '/dev/gpiochip0', '/dev/mem', '/dev/port'):
        require(not os.path.lexists(path), 'host control path visible: ' + path)
    devices = device_inventory()
    marker = Path(args.shm_marker)
    require(marker.parent == Path('/dev/shm') and not marker.is_symlink(), 'invalid test SHM marker')
    metadata = marker.stat()
    require(stat.S_ISREG(metadata.st_mode) and metadata.st_size <= 1024,
            'test SHM marker must be a small regular file')
    require((metadata.st_dev, metadata.st_ino) == (args.marker_dev, args.marker_ino),
            'service does not see the pinned shared-memory backing')
    require(list(marker.parent.iterdir()) == [marker], 'unexpected existing shared-memory contents')
    require(hashlib.sha256(marker.read_bytes()).hexdigest() == args.marker_sha256,
            'private shared-memory marker mismatch')
    denied = {'can': blocked_socket(socket.AF_CAN, socket.SOCK_RAW, socket.CAN_RAW),
              'packet': blocked_socket(socket.AF_PACKET, socket.SOCK_RAW),
              'rawInet': blocked_socket(socket.AF_INET, socket.SOCK_RAW, socket.IPPROTO_ICMP)}
    host_root_denial = blocked_host_root()
    return {'format': 'board-control-isolation-proof-v1', 'passed': True,
            'networkNamespace': current, 'hostNetworkNamespace': args.host_netns,
            'ipcNamespace': current_ipc, 'hostIpcNamespace': args.host_ipcns,
            'links': links, 'routes': routes, 'processSecurity': status,
            'devices': devices, 'socketDenials': denied, 'hostRootDenial': host_root_denial,
            'shmMarker': {'path': str(marker), 'sha256': args.marker_sha256,
                          'dev': metadata.st_dev, 'ino': metadata.st_ino},
            'physicalControlWrites': 0}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host-netns', required=True)
    parser.add_argument('--shared-netns', required=True)
    parser.add_argument('--host-ipcns', required=True)
    parser.add_argument('--shared-ipcns', required=True)
    parser.add_argument('--shm-marker', required=True)
    parser.add_argument('--marker-sha256', required=True)
    parser.add_argument('--marker-dev', type=int, required=True)
    parser.add_argument('--marker-ino', type=int, required=True)
    args = parser.parse_args()
    print(json.dumps(probe(args), sort_keys=True))


if __name__ == '__main__':
    main()
