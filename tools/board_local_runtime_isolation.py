#!/usr/bin/env python3
"""Pre-start proof for canonical services in the no-physical-I/O board profile.

The privileged production guard runs separately; this check and the business
process must have the same systemd sandbox. It does not grant role admission.
"""
import json
from pathlib import Path
import socket
import subprocess

import board_control_isolation_probe as isolation


def verify():
    security = isolation.check_status(Path('/proc/self/status').read_text())
    links = json.loads(subprocess.check_output(['ip', '-j', 'link', 'show'], timeout=10))
    routes = []
    for family in ('-4', '-6'):
        routes += json.loads(subprocess.check_output(['ip', '-j', family, 'route', 'show', 'table', 'all'], timeout=10))
    isolation.check_links(links, routes)
    devices = isolation.device_inventory()
    mounts = isolation.mount_rows(Path('/proc/self/mountinfo').read_text())
    system = [row for row in mounts if row['path'] == '/sys' or row['path'].startswith('/sys/')]
    isolation.require(system and all('ro' in row['options'] for row in system), 'writable sysfs')
    denied = {'can': isolation.blocked_socket(socket.AF_CAN, socket.SOCK_RAW, socket.CAN_RAW),
              'packet': isolation.blocked_socket(socket.AF_PACKET, socket.SOCK_RAW),
              'rawInet': isolation.blocked_socket(socket.AF_INET, socket.SOCK_RAW, socket.IPPROTO_ICMP)}
    return dict(passed=True, processSecurity=security, links=links, routes=routes,
                devices=devices, socketDenials=denied, controlEnabled=False)


if __name__ == '__main__':
    print(json.dumps(verify(), sort_keys=True))
