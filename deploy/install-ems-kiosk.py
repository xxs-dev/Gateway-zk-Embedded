#!/usr/bin/env python3
"""Opt-in GDM kiosk profile for an already verified native EMS deployment."""
import argparse
import configparser
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import pwd
import re
import shutil
import stat
import subprocess


HOME = Path('/opt/modbus-gateway')
HELPER = Path('/usr/local/libexec/gateway-ems-kiosk.py')
WATCH = 'gateway-ems-kiosk-watch.service'
SESSION = 'gateway-ems-kiosk'


def run(args):
    return subprocess.run(args, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          universal_newlines=True, timeout=180).stdout


def sha(raw):
    return hashlib.sha256(raw).hexdigest()


def ini_update(raw, changes):
    config = configparser.ConfigParser(interpolation=None, strict=True)
    config.optionxform = str
    if raw:
        config.read_string(raw)
    for section, values in changes.items():
        if not config.has_section(section):
            config.add_section(section)
        for key, value in values.items():
            config.set(section, key, value)
    out = io.StringIO()
    config.write(out, space_around_delimiters=False)
    return out.getvalue().encode()


def profile(user, helper):
    if not re.fullmatch('[a-z_][a-z0-9_-]{0,31}', user):
        raise ValueError('invalid local session user')
    account = pwd.getpwnam(user)
    if account.pw_uid == 0 or account.pw_uid != 1000:
        raise ValueError('this profile requires the approved UID 1000 Xauthority binding')
    gdm = Path('/etc/gdm3/custom.conf')
    accounts = Path('/var/lib/AccountsService/users') / user
    dmrc = Path(account.pw_dir) / '.dmrc'
    def content(path):
        if path.is_symlink():
            raise ValueError('refusing symlink config: ' + str(path))
        return path.read_text() if path.exists() else ''
    return {
        HELPER: (helper, 0o755, 0, 0),
        Path('/usr/share/xsessions/' + SESSION + '.desktop'): (
            ('[Desktop Entry]\nName=Gateway EMS 2.0\nType=Application\n'
             'Exec=/usr/bin/python3 -B ' + str(HELPER) + ' session\n'
             'TryExec=/usr/bin/python3\nDesktopNames=GatewayEMS;\n').encode(), 0o644, 0, 0),
        gdm: (ini_update(content(gdm), {'daemon': {'WaylandEnable': 'false',
            'AutomaticLoginEnable': 'true', 'AutomaticLogin': user,
            'DefaultSession': SESSION + '.desktop'}}), 0o644, 0, 0),
        accounts: (ini_update(content(accounts), {'User': {
            'Session': SESSION, 'XSession': SESSION}}), 0o600, 0, 0),
        dmrc: (ini_update(content(dmrc), {'Desktop': {'Session': SESSION}}),
               0o600, account.pw_uid, account.pw_gid),
        Path('/etc/systemd/system/gdm.service.d/80-gateway-ems-kiosk.conf'): (
            ('[Service]\nExecStartPost=/usr/bin/python3 -B ' + str(HELPER) +
             ' ready --user ' + user + ' --timeout 75\nTimeoutStartSec=100\n').encode(), 0o644, 0, 0),
        Path('/etc/systemd/system/' + WATCH): (
            ('[Unit]\nDescription=Dedicated EMS display availability monitor\n'
             'Requires=display-manager.service\nWants=gateway-services.service\n'
             'After=display-manager.service gateway-services.service\n'
             '[Service]\nType=simple\nUser=root\n'
             'ExecStart=/usr/bin/python3 -B ' + str(HELPER) + ' watch --user ' + user + '\n'
             'Restart=always\nRestartSec=5\nNoNewPrivileges=yes\n'
             'CapabilityBoundingSet=CAP_DAC_READ_SEARCH\nAmbientCapabilities=\nPrivateDevices=yes\n'
             'ProtectSystem=strict\nProtectHome=tmpfs\nBindReadOnlyPaths=/run/user/1000\nProtectKernelTunables=yes\n'
             'ProtectKernelModules=yes\nProtectControlGroups=yes\n'
             'RestrictAddressFamilies=AF_UNIX\n'
             '[Install]\nWantedBy=graphical.target\n').encode(), 0o644, 0, 0),
        Path('/etc/X11/xorg.conf.d/90-gateway-ems-kiosk.conf'): (
            b'Section "ServerFlags"\n    Option "DontVTSwitch" "true"\n'
            b'    Option "DontZap" "true"\n    Option "BlankTime" "0"\n'
            b'    Option "StandbyTime" "0"\n    Option "SuspendTime" "0"\n'
            b'    Option "OffTime" "0"\nEndSection\n', 0o644, 0, 0),
    }


def guard_context():
    path = HOME / 'bin/runtime-upgrade-guard.py'
    spec = importlib.util.spec_from_file_location('kiosk_guard', path)
    guard = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(guard)
    baseline = guard.production_context(HOME)
    if 'ky-ems.service' not in baseline['allowedUnits'] or baseline['controlEnabled'] is not False:
        raise ValueError('kiosk requires an approved, control-disabled native display baseline')
    env = (HOME / 'config/runtime/qt-display.env').read_text().splitlines()
    if 'DISPLAY=:0' not in env or 'QT_QPA_PLATFORM=xcb' not in env:
        raise ValueError('native XCB display environment required')
    unit = run(['systemctl', 'cat', 'ky-ems.service'])
    binding = '/run/user/1000/gdm/Xauthority /run/ky-ems/Xauthority'
    if binding not in unit or 'After=display-manager.service' not in unit:
        raise ValueError('unsupported native service authorization/readiness binding')
    if run(['systemctl', 'show', 'display-manager.service', '-p', 'Id', '--value']).strip() != 'gdm.service':
        raise ValueError('GDM display manager required')
    if run(['systemctl', 'get-default']).strip() != 'graphical.target':
        raise ValueError('graphical.target must already be the boot target')
    for name in ('xset', 'xsetroot', 'xprop', 'xwd', 'Xorg'):
        if not shutil.which(name):
            raise ValueError('missing display dependency: ' + name)
    return baseline


def atomic_write(path, raw, mode, uid, gid):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + '.kiosk-new')
    with temporary.open('xb') as stream:
        stream.write(raw)
        stream.flush()
        os.fsync(stream.fileno())
    os.chmod(str(temporary), mode)
    os.chown(str(temporary), uid, gid)
    os.replace(str(temporary), str(path))


def install(user, backup):
    baseline = guard_context()
    if Path('/etc/systemd/system/' + WATCH).exists():
        raise ValueError('kiosk already installed; preserve and review its existing receipt')
    files = profile(user, Path(__file__).with_name('gateway-ems-kiosk.py').read_bytes())
    backup.mkdir(mode=0o700, parents=True, exist_ok=False)
    rows = []
    for path, (raw, mode, uid, gid) in files.items():
        row = dict(path=str(path), installedSha256=sha(raw), installedMode=mode,
                   installedUid=uid, installedGid=gid, previous=None)
        if path.exists() or path.is_symlink():
            info = path.lstat()
            if not stat.S_ISREG(info.st_mode):
                raise ValueError('refusing non-regular kiosk target: ' + str(path))
            saved = backup / ('file-' + str(len(rows)))
            saved.write_bytes(path.read_bytes())
            saved.chmod(0o600)
            row['previous'] = dict(file=saved.name, sha256=sha(saved.read_bytes()),
                mode=stat.S_IMODE(info.st_mode), uid=info.st_uid, gid=info.st_gid)
        rows.append(row)
    receipt = dict(format='gateway-ems-kiosk-install-v1', user=user,
                   baselineId=baseline['baselineId'], files=rows)
    with (backup / 'receipt.json').open('x') as stream:
        json.dump(receipt, stream, indent=2)
    for path, values in files.items():
        atomic_write(path, *values)
    run(['systemctl', 'daemon-reload'])
    run(['systemctl', 'enable', WATCH])
    # Revalidate the unchanged production baseline; kiosk files are external
    # display infrastructure, never a reason to regenerate or bypass its pins.
    guard_context()
    print(json.dumps(receipt, indent=2))


def restore(backup):
    current = backup / 'receipt-current.json'
    receipt = json.loads((current if current.exists() else backup / 'receipt.json').read_text())
    expected = set(profile(receipt['user'], b'').keys())
    if (receipt['format'] != 'gateway-ems-kiosk-install-v1' or
            {Path(r['path']) for r in receipt['files']} != expected):
        raise ValueError('invalid kiosk restoration paths')
    for row in receipt['files']:
        path = Path(row['path'])
        if path.is_symlink() or sha(path.read_bytes()) != row['installedSha256']:
            raise ValueError('kiosk file changed; refusing to overwrite: ' + str(path))
        if row['previous']:
            saved = backup / row['previous']['file']
            if saved.parent != backup or sha(saved.read_bytes()) != row['previous']['sha256']:
                raise ValueError('backup digest/path mismatch')
    run(['systemctl', 'disable', '--now', WATCH])
    run(['systemctl', 'stop', 'ky-ems.service'])
    for row in receipt['files']:
        path = Path(row['path'])
        old = row['previous']
        if old:
            atomic_write(path, (backup / old['file']).read_bytes(), old['mode'], old['uid'], old['gid'])
        else:
            path.unlink()
    run(['systemctl', 'daemon-reload'])
    run(['systemctl', 'restart', 'accounts-daemon.service'])
    run(['systemctl', 'restart', 'gdm.service'])
    print('Display profile restored. New KY-EMS remains stopped; old programs were not started.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('check', 'install', 'restore'))
    parser.add_argument('--user', default='szrl')
    parser.add_argument('--backup', type=Path)
    args = parser.parse_args()
    if os.geteuid() != 0:
        parser.error('root required')
    if args.action == 'check':
        print(json.dumps(dict(baselineId=guard_context()['baselineId'],
            files=[str(p) for p in profile(args.user, b'')]), indent=2))
    else:
        if args.backup is None or not args.backup.is_absolute():
            parser.error('--backup must be an absolute new protected directory')
        if args.action == 'install':
            install(args.user, args.backup)
        else:
            restore(args.backup)


if __name__ == '__main__':
    main()
