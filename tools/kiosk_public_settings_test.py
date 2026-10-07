"""LOCAL_ONLY: Windows filesystem fixtures/modelled POSIX metadata, not DAC proof."""
import ast
import importlib.util
import json
import copy
import types
import os
import re
import math
import stat
import tempfile
import subprocess
from pathlib import Path, PurePosixPath
import io
import unittest
from unittest import mock

DEPLOY = Path(__file__).resolve().parents[1] / 'deploy'


def helper():
    spec = importlib.util.spec_from_file_location('kiosk_prepare', str(DEPLOY / 'prepare-generic-runtime.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def app():
    return {'auth': {'password': 'PRIVATE_SENTINEL'}, 'identity': 'PRIVATE_ID',
            'localDisplay': {'port': 18123, 'bindHost': 'PRIVATE_HOST',
                             'kiosk': {'width': 1920, 'height': 1080, 'fullscreen': False,
                                       'setDisplayMode': False, 'enabled': True,
                                       'requireDisplayConnected': False, 'requireEdid': False,
                                       'displayOutput': 'HDMI-1', 'zoom': 1.25,
                                       'unknownPrivate': 'PRIVATE_EXTRA'},
                             'pages': [{'pageCode': 'Energy_2', 'private': 'PRIVATE_PAGE'},
                                       {'pageCode': 'PRIVATE_NEXT'}]}}


def sized_app(size):
    value = app()
    value['localDisplay']['enabled'] = True
    value['engineeringOnly'] = ''
    raw = json.dumps(value, separators=(',', ':')).encode('ascii')
    value['engineeringOnly'] = 'x' * (size - len(raw))
    raw = json.dumps(value, separators=(',', ':')).encode('ascii')
    assert len(raw) == size
    return raw


def reader():
    # Actual loader functions, GTK class/imports excluded; no GUI execution claim.
    tree = ast.parse((DEPLOY / 'local-kiosk.py').read_text(encoding='utf-8'))
    selected = [node for node in tree.body if isinstance(node, ast.FunctionDef) or
                (isinstance(node, ast.Assign) and any(isinstance(t, ast.Name) and
                                                     t.id == 'DEFAULT_KIOSK' for t in node.targets))]
    namespace = dict(json=json, os=os, re=re, math=math, stat=stat, Path=Path)
    exec(compile(ast.Module(body=selected, type_ignores=[]), '<actual-kiosk-functions>', 'exec'), namespace)
    return types.SimpleNamespace(**namespace)


class ProjectionTests(unittest.TestCase):
    def test_allowlist_preserves_nondefaults_without_private_fields(self):
        source = app()
        before = json.dumps(source, sort_keys=True)
        result = helper().kiosk_projection(source)
        self.assertEqual({'localDisplay'}, set(result))
        local = result['localDisplay']
        self.assertEqual({'port', 'kiosk', 'pages'}, set(local))
        self.assertEqual(18123, local['port'])
        self.assertEqual([{'pageCode': 'Energy_2'}], local['pages'])
        self.assertEqual(1920, local['kiosk']['width'])
        self.assertFalse(local['kiosk']['fullscreen'])
        self.assertEqual(1.25, local['kiosk']['zoom'])
        self.assertNotIn('PRIVATE', json.dumps(result))
        self.assertEqual(before, json.dumps(source, sort_keys=True))

    def test_invalid_consumed_fields_and_json_fail_closed(self):
        module = helper()
        for field, bad in [('port', True), ('port', 65536), ('kiosk', []), ('pages', {})]:
            value = app()
            value['localDisplay'][field] = bad
            with self.subTest(field=field, bad=bad), self.assertRaises(ValueError):
                module.kiosk_projection(value)
        for key, bad in [('width', '1920'), ('height', False), ('zoom', float('nan')),
                         ('fullscreen', 'false'), ('displayOutput', {'secret': 1})]:
            value = app()
            value['localDisplay']['kiosk'][key] = bad
            with self.subTest(key=key), self.assertRaises(ValueError):
                module.kiosk_projection(value)
        for raw in [b'{"localDisplay":{},"localDisplay":{}}', b'{"x":NaN}', b'{"x":Infinity}', b'{"x":1e999}', b'broken']:
            with self.subTest(raw=raw), self.assertRaisesRegex(ValueError, 'KIOSK_JSON_INVALID'):
                module.kiosk_json(raw)

    def test_read_limit_applies_to_actual_open_handle(self):
        module = helper()
        metadata = types.SimpleNamespace(st_uid=0, st_mode=0o100600, st_nlink=1, st_size=1)
        with mock.patch.object(module.os, 'fstat', return_value=metadata), \
             mock.patch.object(module.os, 'read', side_effect=[b' ' * 1024] * 1025):
            with self.assertRaisesRegex(ValueError, 'KIOSK_INPUT_TOO_LARGE'):
                module.kiosk_read(123, 1024 * 1024)

    def test_public_directory_policy_never_repairs_existing_modes(self):
        module = helper()
        for uid, mode in [(1, 0o40755), (0, 0o40700), (0, 0o40711), (0, 0o40777), (0, 0o120755)]:
            with self.subTest(uid=uid, mode=mode), mock.patch.object(module.os, 'fstat',
                    return_value=types.SimpleNamespace(st_uid=uid, st_mode=mode)), \
                    self.assertRaisesRegex(ValueError, 'KIOSK_PARENT_POLICY'):
                module.kiosk_directory_policy(123, public=True)
        with mock.patch.object(module.os, 'fstat',
                return_value=types.SimpleNamespace(st_uid=0, st_mode=0o40755)):
            module.kiosk_directory_policy(123, public=True)


class ReaderTests(unittest.TestCase):
    def test_managed_argument_only_reads_public_projection(self):
        module = reader()
        raw = json.dumps(helper().kiosk_projection(app())).encode()
        home = Path(tempfile.gettempdir()) / 'model-gateway'
        managed = str(home / 'config/runtime/apps/monitor-service.json')
        seen = []
        def public_read(path):
            seen.append(str(path))
            return raw
        function = module.load_local_display_settings
        with mock.patch.dict(function.__globals__, {'read_kiosk_public': public_read}), \
                mock.patch('builtins.open', side_effect=AssertionError('private open forbidden')):
            page, frame, settings = function(managed)
        self.assertEqual([str(home / 'kiosk/monitor-service.json')], seen)
        self.assertEqual('http://127.0.0.1:18123/?pageCode=Energy_2', page)
        self.assertEqual('http://127.0.0.1:18123/api/frame?pageCode=Energy_2', frame)
        self.assertFalse(settings['fullscreen'])

    def test_nonmanaged_missing_and_malformed_do_not_fallback(self):
        module = reader()
        load = module.load_local_display_settings
        for path in ['/tmp/private.json', '/opt/modbus-gateway/config/runtime/apps/../private.json']:
            with self.subTest(path=path), mock.patch('builtins.open', side_effect=AssertionError('private open')), \
                    self.assertRaises(ValueError):
                load(str(Path(path)))
        managed = str(Path(tempfile.gettempdir()) / 'model-gateway/config/runtime/apps/monitor-service.json')
        for raw in [b'{}', b'broken', b'{"localDisplay":{"port":18123}}']:
            with self.subTest(raw=raw), mock.patch.dict(load.__globals__, {'read_kiosk_public': lambda path: raw}), \
                    self.assertRaises(ValueError):
                load(managed)
        with mock.patch.dict(load.__globals__, {'read_kiosk_public': mock.Mock(side_effect=OSError())}), \
                self.assertRaises(ValueError):
            load(managed)


class ServiceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='kiosk-shell-')
        self.addCleanup(self.temp.cleanup)
        self.tree = Path(self.temp.name)
        self.home = self.tree / 'home'
        for name in ['bin', 'data', 'config/runtime/apps', 'kiosk', 'tmp', 'commands']:
            (self.home / name).mkdir(parents=True, exist_ok=True)
        self.log = self.tree / 'calls.log'
        self.source = self.home / 'config/runtime/apps/monitor-service.json'
        value = app()
        value['localDisplay']['enabled'] = True
        self.source.write_text(json.dumps(value), encoding='utf-8')
        self.output = self.home / 'kiosk/monitor-service.json'
        self.output.write_text('OLD_PROJECTION', encoding='utf-8')
        (self.home / 'bin/prepare-generic-runtime.py').write_text('# fixture entry marker\n', encoding='utf-8')
        # Only process/permission boundaries modelled. Actual production projection
        # and shell control flow run; no systemctl/root/system directories involved.
        dispatch = self.tree / 'python_dispatch.py'
        dispatch.write_text('''import sys, pathlib, json, importlib.util, os, subprocess
sys.stdout.reconfigure(newline='\\n')
args=sys.argv[1:]
if args and args[0].endswith('prepare-generic-runtime.py'):
    with open(os.environ['CALLS_NATIVE'], 'a') as stream: stream.write('PROJECT\\n')
    if os.environ.get('FAIL_PROJECT') == '1': sys.exit(2)
    spec=importlib.util.spec_from_file_location('p', os.environ['REAL_HELPER'])
    p=importlib.util.module_from_spec(spec); spec.loader.exec_module(p)
    home=pathlib.Path(args[args.index('--home')+1]); instance=args[args.index('--instance')+1]
    p.kiosk_instance(instance)
    raw=(home/'config/runtime/apps'/ (instance+'.json')).read_bytes()
    value=p.kiosk_projection(p.kiosk_json(raw))
    (home/'kiosk'/ (instance+'.json')).write_text(json.dumps(value))
elif args and args[0].endswith('runtime-upgrade-guard.py'):
    assert args[1]=='activated-list'
    print('local-kiosk@monitor-service.service'); print('ky-ems.service')
else:
    result=subprocess.run([sys.executable, '-B']+args, input=sys.stdin.buffer.read(), capture_output=True, timeout=10)
    sys.stdout.write(result.stdout.decode().replace('\\r\\n', '\\n'))
    sys.stderr.write(result.stderr.decode())
    sys.exit(result.returncode)
''', encoding='utf-8')
        commands = self.home / 'commands'
        (commands / 'python3').write_text('#!/bin/sh\nexec "$PYTHON_EXE" -B "$DISPATCH" "$@"\n', encoding='utf-8')
        (commands / 'systemctl').write_text('#!/bin/sh\nprintf "SYSTEMCTL %s\\n" "$*" >> "$CALLS"\n', encoding='utf-8')
        self.env = dict(os.environ, PYTHON_EXE=os.sys.executable, DISPATCH=str(dispatch), CALLS=str(self.log), CALLS_NATIVE=str(self.log),
                        REAL_HELPER=str(DEPLOY / 'prepare-generic-runtime.py'),
                        FIXTURE_HOME=str(self.home), COMMANDS=str(commands))
        self.bash = 'C:/Program Files/Git/usr/bin/bash.exe' if os.name == 'nt' else '/bin/bash'

    def shell(self, action):
        source = Path(os.environ.get('KIOSK_SERVICE_SOURCE', str(DEPLOY / 'gateway-services.sh'))).read_text(encoding='utf-8')
        source = source[:source.rindex('case "${1:-apply}" in')]
        fixture = self.tree / 'gateway-selected.sh'
        fixture.write_text('''PATH="$(/usr/bin/cygpath -u "$COMMANDS"):/usr/bin:/bin"; export PATH
GATEWAY_HOME="$(/usr/bin/cygpath -u "$FIXTURE_HOME")"; export GATEWAY_HOME
CALLS="$(/usr/bin/cygpath -u "$CALLS")"; export CALLS
TMPDIR="$GATEWAY_HOME/tmp"; export TMPDIR
''' + source + '\n' + ('listed_units' if action == 'list' else 'start_units') + '\n', encoding='utf-8')
        result = subprocess.run([self.bash, str(fixture)], env=self.env, capture_output=True, timeout=20)
        print('SHELL', action, result.returncode, repr(result.stdout), repr(result.stderr), flush=True)
        return result

    def calls(self):
        return self.log.read_text().splitlines() if self.log.exists() else []

    def test_list_read_only_and_next_start_refreshes_source_edit(self):
        result = self.shell('list')
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertIn(b'local-kiosk@monitor-service.service', result.stdout)
        self.assertEqual([], self.calls())
        self.assertEqual('OLD_PROJECTION', self.output.read_text())
        result = self.shell('start')
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertEqual(['SYSTEMCTL start local-display@monitor-service.service', 'PROJECT',
                          'SYSTEMCTL start --no-block local-kiosk@monitor-service.service'], self.calls())
        self.assertEqual(18123, json.loads(self.output.read_text())['localDisplay']['port'])
        value = app(); value['localDisplay']['enabled'] = True
        value['localDisplay']['port'] = 18456
        self.source.write_text(json.dumps(value))
        result = self.shell('start')
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertEqual(18456, json.loads(self.output.read_text())['localDisplay']['port'])

    def test_activated_list_refresh_failure_blocks_only_kiosk_and_keeps_old_bytes(self):
        (self.home / 'data/runtime-upgrade-stop').write_text('fixture')
        result = self.shell('list')
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertEqual([], self.calls())
        self.env['FAIL_PROJECT'] = '1'
        result = self.shell('start')
        self.assertEqual(1, result.returncode, result.stderr)
        self.assertEqual(['PROJECT', 'SYSTEMCTL start --no-block ky-ems.service'], self.calls())
        self.assertEqual('OLD_PROJECTION', self.output.read_text())
        self.env['FAIL_PROJECT'] = '0'
        result = self.shell('start')
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertEqual(18123, json.loads(self.output.read_text())['localDisplay']['port'])


class PosixModel:
    """In-memory syscall contract, not actual owner/no-follow/DAC enforcement."""
    name = 'posix'
    O_RDONLY, O_WRONLY, O_CREAT, O_EXCL = 0, 1, 2, 4
    O_NOFOLLOW, O_CLOEXEC, O_NONBLOCK, O_DIRECTORY = 8, 16, 32, 64

    def __init__(self, umask=0o077):
        self.umask = umask
        self.entries = {}
        self.handles = {}
        self.events = []
        self.next_fd = 100
        self.add('/', stat.S_IFDIR | 0o755)
        self.add('/opt', stat.S_IFDIR | 0o755)
        self.add('/source', stat.S_IFDIR | 0o700)
        self.add('/source/local-kiosk.py', stat.S_IFREG | 0o600, data=b'PUBLIC_CODE')

    def add(self, path, mode, uid=0, data=b'', nlink=1):
        self.entries[path] = dict(mode=mode, uid=uid, data=data, nlink=nlink)

    def geteuid(self):
        return 0

    def absolute(self, name, dir_fd=None):
        return str(PurePosixPath(self.handles[dir_fd]['path']) / name) if dir_fd is not None else str(name)

    def open(self, name, flags, mode=0o777, dir_fd=None):
        assert flags & self.O_NOFOLLOW
        path = self.absolute(name, dir_fd)
        if flags & self.O_CREAT:
            if path in self.entries and flags & self.O_EXCL:
                raise FileExistsError()
            self.add(path, stat.S_IFREG | (mode & ~self.umask))
            self.events.append(('create', path))
        if path not in self.entries:
            raise FileNotFoundError()
        entry = self.entries[path]
        if stat.S_ISLNK(entry['mode']):
            raise OSError('model nofollow')
        if flags & self.O_DIRECTORY and not stat.S_ISDIR(entry['mode']):
            raise OSError('model not directory')
        fd = self.next_fd; self.next_fd += 1
        self.handles[fd] = dict(path=path, entry=entry, offset=0)
        self.events.append(('open', path))
        return fd

    def fstat(self, fd):
        entry = self.handles[fd]['entry']
        return types.SimpleNamespace(st_uid=entry['uid'], st_mode=entry['mode'], st_nlink=entry['nlink'],
                                     st_size=len(entry['data']), st_mtime_ns=1, st_ctime_ns=1)

    def read(self, fd, size):
        handle = self.handles[fd]
        chunk = handle['entry']['data'][handle['offset']:handle['offset'] + size]
        handle['offset'] += len(chunk)
        return chunk

    def close(self, fd):
        del self.handles[fd]

    def listdir(self, fd):
        prefix = self.handles[fd]['path'].rstrip('/') + '/'
        return [path[len(prefix):] for path in self.entries
                if path.startswith(prefix) and '/' not in path[len(prefix):]]

    def mkdir(self, name, mode, dir_fd):
        path = self.absolute(name, dir_fd)
        if path in self.entries:
            raise FileExistsError()
        self.add(path, stat.S_IFDIR | (mode & ~self.umask))
        self.events.append(('mkdir', path))

    def fchmod(self, fd, mode):
        self.handles[fd]['entry']['mode'] = stat.S_IFMT(self.handles[fd]['entry']['mode']) | mode
        self.events.append(('fchmod', self.handles[fd]['path'], mode))

    def fsync(self, fd):
        self.events.append(('fsync', self.handles[fd]['path']))

    def fdopen(self, fd, mode):
        model = self
        class Stream(io.BytesIO):
            def fileno(self):
                return fd
            def close(self):
                if not self.closed:
                    model.handles[fd]['entry']['data'] = self.getvalue()
                    model.close(fd)
                super().close()
        return Stream()

    def replace(self, source, target, src_dir_fd, dst_dir_fd):
        self.entries[self.absolute(target, dst_dir_fd)] = self.entries.pop(self.absolute(source, src_dir_fd))
        self.events.append(('replace', self.absolute(target, dst_dir_fd)))

    def unlink(self, name, dir_fd):
        path = self.absolute(name, dir_fd)
        if path not in self.entries:
            raise FileNotFoundError()
        del self.entries[path]


class FilesystemContractTests(unittest.TestCase):
    def fixture(self, umask=0o077):
        module, fs = helper(), PosixModel(umask)
        module.os, module.Path = fs, PurePosixPath
        args = types.SimpleNamespace(home=PurePosixPath('/opt/modbus-gateway'), instance='monitor-service',
                                     source=PurePosixPath('/source/local-kiosk.py'))
        return module, fs, args

    def private_app(self, fs, raw=None):
        for suffix in ['config', 'config/runtime', 'config/runtime/apps', 'ota']:
            fs.add('/opt/modbus-gateway/' + suffix, stat.S_IFDIR | 0o700)
        fs.add('/opt/modbus-gateway/config/runtime/apps/monitor-service.json', stat.S_IFREG | 0o600,
               data=raw if raw is not None else json.dumps(app()).encode())
        fs.add('/opt/modbus-gateway/ota/fence', stat.S_IFREG | 0o600, data=b'PRIVATE_FENCE')

    def test_real_helper_flow_exact_modes_private_preservation_and_reader(self):
        for umask in (0o022, 0o077):
            with self.subTest(umask=umask):
                module, fs, args = self.fixture(umask)
                module.check_kiosk_parents(args)
                self.assertFalse(any(e[0] in ('mkdir', 'fchmod') for e in fs.events))
                module.prepare_kiosk_parents(args)
                self.private_app(fs)
                private = {key: copy.deepcopy(value) for key, value in fs.entries.items()
                           if '/config' in key or '/ota' in key}
                module.install_kiosk_script(args)
                module.project_kiosk(args)
                for name in ['', '/bin', '/kiosk']:
                    self.assertEqual(0o755, stat.S_IMODE(fs.entries['/opt/modbus-gateway' + name]['mode']))
                self.assertEqual(0o755, stat.S_IMODE(fs.entries['/opt/modbus-gateway/bin/local-kiosk.py']['mode']))
                public = fs.entries['/opt/modbus-gateway/kiosk/monitor-service.json']
                self.assertEqual(0o644, stat.S_IMODE(public['mode']))
                self.assertNotIn(b'PRIVATE', public['data'])
                self.assertEqual(private, {key: value for key, value in fs.entries.items()
                                         if '/config' in key or '/ota' in key})
                fs.events.clear()
                consumer = reader().load_local_display_settings
                with mock.patch.dict(consumer.__globals__, {'os': fs, 'Path': PurePosixPath}):
                    page, _, settings = consumer('/opt/modbus-gateway/config/runtime/apps/monitor-service.json')
                self.assertIn(':18123/', page)
                self.assertEqual(1920, settings['width'])
                self.assertFalse(any('/config' in e[1] for e in fs.events))
                self.assertEqual({}, fs.handles)

    def test_restrictive_foreign_link_parents_fail_before_any_mutation(self):
        for target in ['/opt', '/opt/modbus-gateway', '/opt/modbus-gateway/bin', '/opt/modbus-gateway/kiosk']:
            for mode, uid in [(0o40700, 0), (0o40755, 1000), (0o120755, 0), (0o40777, 0)]:
                with self.subTest(target=target, mode=mode, uid=uid):
                    module, fs, args = self.fixture()
                    fs.add('/opt/modbus-gateway', stat.S_IFDIR | 0o755)
                    fs.add(target, mode, uid=uid)
                    before = copy.deepcopy(fs.entries)
                    with self.assertRaises((ValueError, OSError)):
                        module.prepare_kiosk_parents(args)
                    self.assertEqual(before, fs.entries)
                    self.assertEqual({}, fs.handles)

    def test_invalid_private_input_and_output_links_preserve_previous_projection(self):
        for bad in [b'broken', b'{"localDisplay":{},"localDisplay":{}}', b'{"private":NaN}', sized_app(5 * 1024 * 1024 + 1)]:
            with self.subTest(kind=bad[:32]):
                module, fs, args = self.fixture()
                module.prepare_kiosk_parents(args); self.private_app(fs, bad)
                fs.add('/opt/modbus-gateway/kiosk/monitor-service.json', stat.S_IFREG | 0o644, data=b'OLD')
                before = copy.deepcopy(fs.entries)
                with self.assertRaises(ValueError): module.project_kiosk(args)
                self.assertEqual(before, fs.entries)
                self.assertEqual({}, fs.handles)
        for target in ['config/runtime/apps/monitor-service.json', 'kiosk/monitor-service.json']:
            module, fs, args = self.fixture()
            module.prepare_kiosk_parents(args); self.private_app(fs)
            fs.add('/opt/modbus-gateway/' + target, stat.S_IFLNK | 0o777, data=b'LINK')
            before = copy.deepcopy(fs.entries)
            with self.assertRaises(OSError): module.project_kiosk(args)
            self.assertEqual(before, fs.entries)
            self.assertEqual({}, fs.handles)


    def test_original_app_five_mib_both_paths_and_previous_public_preserved(self):
        for size in (1024 * 1024 + 17, 5 * 1024 * 1024, 5 * 1024 * 1024 + 1):
            with self.subTest(size=size):
                raw = sized_app(size)
                module, fs, args = self.fixture()
                module.prepare_kiosk_parents(args); self.private_app(fs, raw)
                args.source = PurePosixPath('/source/monitor-service.json')
                fs.add(str(args.source), stat.S_IFREG | 0o600, data=raw)
                output = '/opt/modbus-gateway/kiosk/monitor-service.json'
                fs.add(output, stat.S_IFREG | 0o644, data=b'OLD_PUBLIC')
                before = copy.deepcopy(fs.entries)
                if size > 5 * 1024 * 1024:
                    for action in (module.check_kiosk_app, module.project_kiosk):
                        with self.assertRaisesRegex(ValueError, 'KIOSK_INPUT_TOO_LARGE'):
                            action(args)
                        self.assertEqual(before, fs.entries)
                else:
                    module.check_kiosk_app(args)
                    self.assertEqual(before, fs.entries)
                    module.project_kiosk(args)
                    public = fs.entries[output]['data']
                    self.assertEqual(module.kiosk_projection(json.loads(raw)), json.loads(public))
                    self.assertLess(len(public), 8192)
                    self.assertNotIn(b'engineeringOnly', public)
                    self.assertNotIn(b'PRIVATE', public)
                    self.assertEqual(before[str(args.source)], fs.entries[str(args.source)])
                    private = '/opt/modbus-gateway/config/runtime/apps/monitor-service.json'
                    self.assertEqual(before[private], fs.entries[private])
                self.assertEqual({}, fs.handles)

    def test_original_app_json_and_source_policies_not_weakened(self):
        invalid = (b'broken', b'{"localDisplay":{},"localDisplay":{}}', b'{"private":NaN}')
        cases = [(raw, 0, stat.S_IFREG | 0o600, 1, 'KIOSK_JSON_INVALID') for raw in invalid]
        cases += [(sized_app(1024 * 1024 + 17), uid, mode, links, cause)
                  for uid, mode, links, cause in (
                      (1000, stat.S_IFREG | 0o600, 1, 'KIOSK_FILE_POLICY'),
                      (0, stat.S_IFREG | 0o622, 1, 'KIOSK_FILE_POLICY'),
                      (0, stat.S_IFREG | 0o600, 2, 'KIOSK_FILE_POLICY'),
                      (0, stat.S_IFLNK | 0o777, 1, 'model nofollow'))]
        for raw, uid, mode, links, cause in cases:
            for action_name in ('check_kiosk_app', 'project_kiosk'):
                with self.subTest(action=action_name, uid=uid, mode=mode, links=links, cause=cause):
                    module, fs, args = self.fixture()
                    module.prepare_kiosk_parents(args); self.private_app(fs, raw)
                    args.source = PurePosixPath('/source/monitor-service.json')
                    path = str(args.source) if action_name == 'check_kiosk_app' else '/opt/modbus-gateway/config/runtime/apps/monitor-service.json'
                    fs.add(path, mode, uid=uid, data=raw, nlink=links)
                    fs.add('/opt/modbus-gateway/kiosk/monitor-service.json', stat.S_IFREG | 0o644, data=b'OLD_PUBLIC')
                    before = copy.deepcopy(fs.entries)
                    with self.assertRaisesRegex((ValueError, OSError), cause):
                        getattr(module, action_name)(args)
                    self.assertEqual(before, fs.entries)
                    self.assertEqual({}, fs.handles)

    def test_script_and_nonroot_public_reader_limits_unchanged(self):
        module, fs, args = self.fixture()
        fs.add(str(args.source), stat.S_IFREG | 0o600, data=b'#' * (256 * 1024 + 1))
        with self.assertRaisesRegex(ValueError, 'KIOSK_INPUT_TOO_LARGE'):
            module.check_kiosk_source(args)
        module.prepare_kiosk_parents(args)
        fs.add('/opt/modbus-gateway/kiosk/monitor-service.json', stat.S_IFREG | 0o644, data=b' ' * 8193)
        consumer = reader().read_kiosk_public
        with mock.patch.dict(consumer.__globals__, {'os': fs, 'Path': PurePosixPath}):
            with self.assertRaisesRegex(ValueError, 'KIOSK_PUBLIC_TOO_LARGE'):
                consumer(PurePosixPath('/opt/modbus-gateway/kiosk/monitor-service.json'))
        self.assertEqual({}, fs.handles)


class InstallerTests(ServiceTests):
    # Keep only the installer-specific selectors; inherited service cases not
    # counted again in final affected selection.
    def test_parent_rejection_precedes_guard_target_writes_and_service_operations(self):
        commands = self.home / 'commands'
        (commands / 'python3').write_text('''#!/bin/sh
printf 'PY %s\\n' "$*" >> "$CALLS"
case "$*" in *check-kiosk-parents*) echo KIOSK_PARENT_POLICY >&2; exit 2;; esac
exit 0
''', encoding='utf-8')
        fixture = self.tree / 'installer-selected.sh'
        fixture.write_text('''PATH="$(/usr/bin/cygpath -u "$COMMANDS"):/usr/bin:/bin"; export PATH
CALLS="$(/usr/bin/cygpath -u "$CALLS")"; export CALLS
GATEWAY_HOME="$(/usr/bin/cygpath -u "$FIXTURE_HOME")"; export GATEWAY_HOME
INSTALL_SYSTEMD=0; START_SERVICES=0; export INSTALL_SYSTEMD START_SERVICES
''' + (DEPLOY / 'install-factory-config.sh').read_text(encoding='utf-8'), encoding='utf-8')
        before = self.output.read_bytes()
        result = subprocess.run([self.bash, str(fixture)], env=self.env, capture_output=True, timeout=20)
        self.assertEqual(2, result.returncode, result.stderr)
        self.assertIn(b'KIOSK_PARENT_POLICY', result.stderr)
        self.assertEqual(1, len(self.calls()))
        self.assertIn('check-kiosk-parents', self.calls()[0])
        self.assertEqual(before, self.output.read_bytes())

    def test_projection_order_and_public_script_atomic_command(self):
        source = (DEPLOY / 'install-factory-config.sh').read_text(encoding='utf-8')
        check = source.index(' check-kiosk-parents --home ')
        self.assertLess(check, source.index(' factory "$GATEWAY_HOME"'))
        self.assertLess(check, source.index('mkdir -p "$GATEWAY_HOME/config"'))
        self.assertIn('install-kiosk-script \\\n  --source "$KIOSK_SCRIPT_SOURCE" --home "$GATEWAY_HOME"', source)
        projection = source.index(' project-kiosk \\\n  --home "$GATEWAY_HOME" --instance monitor-service')
        self.assertGreater(projection, source.rindex('apply_direct_maintenance_config \\\n'))
        self.assertLess(projection, source.index('  systemctl start gateway-services.service'))


class CompatibilityTests(unittest.TestCase):
    def test_changed_python_syntax_is_36_compatible_and_shell_syntax_valid(self):
        for name in ['prepare-generic-runtime.py', 'local-kiosk.py']:
            tree = ast.parse((DEPLOY / name).read_text(encoding='utf-8'), feature_version=(3, 6))
            self.assertTrue(tree.body)
        bash = 'C:/Program Files/Git/usr/bin/bash.exe' if os.name == 'nt' else '/bin/bash'
        for name in ['gateway-services.sh', 'install-factory-config.sh']:
            result = subprocess.run([bash, '-n', str(DEPLOY / name)], capture_output=True, timeout=10)
            self.assertEqual(0, result.returncode, result.stderr)

    def test_public_contract_exact_keys_and_optional_defaults_agree(self):
        projection = helper().kiosk_projection({'localDisplay': {}})
        module = reader()
        self.assertEqual(module.DEFAULT_KIOSK, projection['localDisplay']['kiosk'])
        port, page, _ = module.parse_kiosk_public(json.dumps(projection).encode())
        self.assertEqual((18080, 'overview'), (port, page))
        for mutation in ['unknown', 'missing', 'bool_port', 'nan', 'duplicate']:
            value = copy.deepcopy(projection)
            if mutation == 'unknown': value['private'] = 'sentinel'
            if mutation == 'missing': del value['localDisplay']['kiosk']['zoom']
            if mutation == 'bool_port': value['localDisplay']['port'] = True
            if mutation == 'nan': value['localDisplay']['kiosk']['zoom'] = float('nan')
            raw = json.dumps(value).encode()
            if mutation == 'duplicate': raw = b'{"localDisplay":{},"localDisplay":{}}'
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                module.parse_kiosk_public(raw)


class TlsAppContractTests(unittest.TestCase):
    def fixture(self, raw=None):
        module, fs = helper(), PosixModel()
        fs.add('/source/apps', stat.S_IFDIR | 0o700)
        value = app(); value['mqtt'] = {'tls': {'sitePolicy': [1, 'retain']}}
        fs.add('/source/apps/monitor-service.json', stat.S_IFREG | 0o600,
               data=raw if raw is not None else json.dumps(value).encode())
        args = types.SimpleNamespace(app_config=PurePosixPath('/source/apps/mqtt-service.json'),
                                     ca_file='/synthetic/ca.crt', cert_file='/synthetic/client.pem',
                                     key_file='/synthetic/client.key', require_prepared=False)
        module.os, module.Path = fs, PurePosixPath
        return module, fs, args

    def test_check_only_retains_bytes_and_all_candidate_policies(self):
        module, fs, args = self.fixture()
        fs.add('/source/apps/aaa.json', stat.S_IFREG | 0o600, data=b'{"mqtt":{}}')
        args.check_only = True
        before = copy.deepcopy(fs.entries)
        module.prepare_tls_apps(args)
        self.assertEqual(before, fs.entries)
        self.assertFalse(any(e[0] in ('create', 'replace', 'fchmod') for e in fs.events))
        args.require_prepared = True
        with self.assertRaisesRegex(ValueError, 'TLS_APP_NOT_PREPARED'):
            module.prepare_tls_apps(args)
        self.assertEqual(before, fs.entries)
        args.require_prepared = False
        fs.entries['/source/apps/monitor-service.json']['data'] = b'{"mqtt":{},"mqtt":{}}'
        changed = copy.deepcopy(fs.entries)
        with self.assertRaisesRegex(ValueError, 'KIOSK_JSON_INVALID'):
            module.prepare_tls_apps(args)
        self.assertEqual(changed, fs.entries)
        self.assertFalse(fs.handles)

    def test_exact_limit_unicode_idempotence_and_all_fields(self):
        module = helper()
        value = app(); value['mqtt'] = {'tls': {'sitePolicy': [1, 'retain']}}
        value['notes'] = '\u5b57' * 100
        value['engineeringOnly'] = ''
        paths = ('/synthetic/ca.crt', '/synthetic/client.pem', '/synthetic/client.key')
        base = module.tls_app_bytes(json.dumps(value).encode(), *paths, monitor=True)
        value['engineeringOnly'] = 'x' * (module.KIOSK_APP_MAX_BYTES - len(base))
        encoded = module.tls_app_bytes(json.dumps(value, ensure_ascii=False).encode(), *paths, monitor=True)
        self.assertEqual(module.KIOSK_APP_MAX_BYTES, len(encoded))
        self.assertEqual(encoded, module.tls_app_bytes(encoded, *paths, monitor=True))
        decoded = json.loads(encoded)
        self.assertEqual(value['notes'], decoded['notes'])
        self.assertEqual(value['localDisplay'], decoded['localDisplay'])
        self.assertEqual([1, 'retain'], decoded['mqtt']['tls']['sitePolicy'])
        # Still-valid input just below the source bound, but final +1 byte.
        value['engineeringOnly'] += 'x'
        with self.assertRaisesRegex(ValueError, 'KIOSK_INPUT_TOO_LARGE'):
            module.tls_app_bytes(json.dumps(value, ensure_ascii=False, separators=(',', ':')).encode(), *paths, monitor=True)

    def test_all_candidates_checked_before_any_publish(self):
        for kind in ('duplicate', 'nonfinite', 'owner', 'writable', 'hardlink', 'symlink', 'oversize', 'parent'):
            with self.subTest(kind=kind):
                raw = b'{"mqtt":{},"mqtt":{}}' if kind == 'duplicate' else b'{"mqtt":{},"value":NaN}' if kind == 'nonfinite' else None
                module, fs, args = self.fixture(raw)
                fs.add('/source/apps/aaa.json', stat.S_IFREG | 0o600, data=b'{"mqtt":{}}')
                entry = fs.entries['/source/apps/monitor-service.json']
                if kind == 'owner': entry['uid'] = 1000
                if kind == 'writable': entry['mode'] |= 0o020
                if kind == 'hardlink': entry['nlink'] = 2
                if kind == 'symlink': entry['mode'] = stat.S_IFLNK | 0o777
                if kind == 'oversize': entry['data'] = sized_app(module.KIOSK_APP_MAX_BYTES + 1)
                if kind == 'parent': fs.entries['/source/apps']['mode'] |= 0o020
                before = copy.deepcopy(fs.entries)
                with self.assertRaises((ValueError, OSError)):
                    module.prepare_tls_apps(args)
                self.assertEqual(before, fs.entries)
                self.assertFalse(fs.handles)

    def test_prepared_provider_checks_without_rewriting_and_rejects_drift(self):
        module, fs, args = self.fixture()
        module.prepare_tls_apps(args)
        before = copy.deepcopy(fs.entries)
        fs.events.clear(); args.require_prepared = True
        module.prepare_tls_apps(args)
        self.assertEqual(before, fs.entries)
        self.assertFalse(any(e[0] in ('create', 'replace', 'fchmod') for e in fs.events))
        value = json.loads(fs.entries['/source/apps/monitor-service.json']['data'])
        value['mqtt']['tls']['enabled'] = False
        fs.entries['/source/apps/monitor-service.json']['data'] = json.dumps(value).encode()
        changed = copy.deepcopy(fs.entries)
        with self.assertRaisesRegex(ValueError, 'TLS_APP_NOT_PREPARED'):
            module.prepare_tls_apps(args)
        self.assertEqual(changed, fs.entries)
        self.assertFalse(fs.handles)

    def test_opened_source_change_rejected_before_publish(self):
        module, fs, args = self.fixture()
        read = fs.read
        changed = [False]
        def mutate(fd, size):
            chunk = read(fd, size)
            if chunk and not changed[0]:
                fs.handles[fd]['entry']['data'] += b' '
                changed[0] = True
            return chunk
        fs.read = mutate
        with self.assertRaisesRegex(ValueError, 'KIOSK_INPUT_TOO_LARGE|KIOSK_INPUT_CHANGED'):
            module.prepare_tls_apps(args)
        self.assertFalse(any(e[0] in ('create', 'replace') for e in fs.events))
        self.assertFalse(fs.handles)


if __name__ == '__main__':
    unittest.main(verbosity=2)
