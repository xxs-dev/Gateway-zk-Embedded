"""Actual private installer shell; Windows POSIX owner/syscall model, not Linux DAC."""
import importlib.util
import ast
import glob
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import tarfile
import unittest

REPO = Path(__file__).resolve().parents[1]
DEPLOY = REPO / 'deploy'
spec = importlib.util.spec_from_file_location('kiosk_contract_model', REPO / 'tools/kiosk_public_settings_test.py')
support = importlib.util.module_from_spec(spec); spec.loader.exec_module(support)
SHELL = 'C:/Program Files/Git/usr/bin/sh.exe'
NAMES = ('gateway-services.sh', 'gateway-health-watchdog.sh', 'gateway-health-watchdog.service',
         'gateway-health-watchdog.default', 'gateway-run.sh', 'gateway-tls-enroll.sh',
         'install-factory-config.sh', 'factory-lifecycle-stage.py', 'production-smoke-test.sh',
         'ota-apply.sh', 'ota-rollback.sh', 'ota-health-result.py', 'install-scada-project.sh',
         'local-kiosk.py', 'prepare-generic-runtime.py', 'runtime-upgrade-guard.py',
         'offline-runtime-upgrade.py', 'production_role_policy.py', 'production-init.sh')


def write(path, raw):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(raw.encode('utf-8') if isinstance(raw, str) else raw)


DISPATCH = r'''import ast, hashlib, importlib.util, json, os, pathlib, stat, sys
sys.stdout.reconfigure(newline='\n')
args=sys.argv[1:]
if args and args[0].endswith('runtime-upgrade-guard.py'):
    assert args[1]=='factory'
    print('FIXTURE_KERNEL_GUARD_NOT_EXECUTED')
    sys.exit(0)
if args and args[0].endswith('prepare-generic-runtime.py') and args[1] in (
        'check-kiosk-parents', 'prepare-kiosk-parents', 'check-kiosk-source',
        'check-kiosk-app', 'install-kiosk-script', 'project-kiosk', 'prepare-tls-apps'):
    spec=importlib.util.spec_from_file_location('test_model', os.environ['MODEL_SOURCE'])
    model=importlib.util.module_from_spec(spec); spec.loader.exec_module(model)
    spec=importlib.util.spec_from_file_location('actual_prepare', args[0])
    p=importlib.util.module_from_spec(spec); spec.loader.exec_module(p)
    fs=model.PosixModel()
    home=pathlib.Path(os.environ['FIXTURE_HOME'])
    mapped=list(args[1:])
    if '--home' in mapped:
        mapped[mapped.index('--home')+1]='/opt/gateway'
    if mapped[0]=='prepare-tls-apps':
        physical_apps=pathlib.Path(mapped[mapped.index('--app-config')+1]).parent
        fs.add('/source/apps',stat.S_IFDIR|0o700)
        for path in physical_apps.glob('*.json'):
            fs.add('/source/apps/'+path.name,stat.S_IFREG|0o600,data=path.read_bytes())
        mapped[mapped.index('--app-config')+1]='/source/apps/mqtt-service.json'
    if home.exists():
        fs.add('/opt/gateway', stat.S_IFDIR|0o755)
        for name in ('bin','kiosk','config','config/runtime','config/runtime/apps'):
            fs.add('/opt/gateway/'+name,stat.S_IFDIR|(0o755 if name in ('bin','kiosk') else 0o700))
    private=home/'config/runtime/apps/monitor-service.json'
    if private.exists():
        fs.add('/opt/gateway/config/runtime/apps/monitor-service.json',stat.S_IFREG|0o600,data=private.read_bytes())
    if '--source' in mapped:
        i=mapped.index('--source')+1
        source=pathlib.Path(mapped[i])
        mapped[i]='/source/input'
        if source.exists():
            uid, mode, links=0, stat.S_IFREG|0o600, 1
            bad=os.environ.get('BAD_SCRIPT_POLICY','') if source.name=='local-kiosk.py' else ''
            if bad=='owner': uid=1000
            if bad=='writable': mode=stat.S_IFREG|0o622
            if bad=='hardlink': links=2
            if bad=='symlink': mode=stat.S_IFLNK|0o777
            fs.add('/source/input',mode,uid=uid,nlink=links,data=source.read_bytes())
    old=home/'kiosk/monitor-service.json'
    if old.exists(): fs.add('/opt/gateway/kiosk/monitor-service.json',stat.S_IFREG|0o644,data=old.read_bytes())
    p.os=fs; p.Path=model.PurePosixPath
    sys.argv=[args[0]]+mapped
    with open(os.environ['HELPER_TRACE'],'a') as stream: stream.write(mapped[0]+'\n')
    def tls_trace(phase):
        trace=os.environ.get('TLS_CONFIG_TRACE')
        if trace and mapped[0]=='prepare-tls-apps':
            with open(trace,'a') as stream:
                stream.write(json.dumps(dict(phase=phase, flags=mapped,
                    apps={path.name:hashlib.sha256(path.read_bytes()).hexdigest() for path in physical_apps.glob('*.json')},
                    certExists=pathlib.Path(mapped[mapped.index('--cert-file')+1]).exists(),
                    caExists=pathlib.Path(mapped[mapped.index('--ca-file')+1]).exists()))+'\n')
    tls_trace('before')
    entry=ast.parse(pathlib.Path(args[0]).read_bytes()).body[-1]
    assert isinstance(entry,ast.If)
    exec(compile(ast.Module(body=entry.body,type_ignores=[]),'<actual-helper-cli>', 'exec'),p.__dict__)
    if mapped[0]=='prepare-tls-apps':
        for path in physical_apps.glob('*.json'):
            path.write_bytes(fs.entries['/source/apps/'+path.name]['data'])
        tls_trace('after')
    if mapped[0]=='prepare-kiosk-parents':
        (home/'bin').mkdir(parents=True,exist_ok=True); (home/'kiosk').mkdir(exist_ok=True)
    if mapped[0]=='install-kiosk-script':
        (home/'bin/local-kiosk.py').write_bytes(fs.entries['/opt/gateway/bin/local-kiosk.py']['data'])
    if mapped[0]=='project-kiosk' and '/opt/gateway/kiosk/monitor-service.json' in fs.entries:
        old.write_bytes(fs.entries['/opt/gateway/kiosk/monitor-service.json']['data'])
else:
    if args and args[0]=='-c':
        sys.argv=['-c']+args[2:]
        exec(compile(args[1],'<actual-scada-inline-python>','exec'),{'__name__':'__main__'})
    elif args and args[0]=='-':
        namespace={'__name__':'__main__'}
        sys.argv=['-']+args[1:]
        source=sys.stdin.buffer.read()
        fixture=os.environ.get('TLS_ENROLLMENT_FIXTURE')
        if fixture:
            import io, ssl, urllib.error, urllib.request
            from unittest.mock import patch
            assert b'/api/config/tls/enrollment/sign' in source
            assert fixture in ('success','http-error','post-drift')
            def controlled_response(request, timeout, context):
                assert request.full_url=='https://public.invalid/api/config/tls/enrollment/sign'
                assert timeout==30 and context is not None
                assert request.get_header('X-edge-tls-enrollment-token')=='SYNTHETIC_NO_AUTHORITY'
                with open(os.environ['TLS_HTTP_TRACE'],'a') as stream: stream.write(fixture+'\n')
                if fixture=='http-error':
                    raise urllib.error.HTTPError(request.full_url,403,'SYNTHETIC_DENIAL',{},io.BytesIO(b'SYNTHETIC_DENIAL'))
                if fixture=='post-drift':
                    path=pathlib.Path(os.environ['FIXTURE_HOME'])/'config/runtime/apps/monitor-service.json'
                    value=json.loads(path.read_bytes()); value['mqtt']['tls']['enabled']=False
                    path.write_text(json.dumps(value),encoding='utf-8')
                    with open(os.environ['TLS_DRIFT_TRACE'],'w') as stream:
                        stream.write(hashlib.sha256(path.read_bytes()).hexdigest())
                pem='-----BEGIN CERTIFICATE-----\nPUBLIC_STUB_NOT_A_CERTIFICATE\n-----END CERTIFICATE-----\n'
                return io.BytesIO(json.dumps(dict(success=True,certificatePem=pem,caPem=pem)).encode())
            with patch.object(ssl,'create_default_context',return_value=object()), patch.object(urllib.request,'urlopen',side_effect=controlled_response):
                exec(compile(source,'<actual-enrollment-inline-python>','exec'),namespace)
        else:
            exec(compile(source,'<actual-installer-inline-python>','exec'),namespace)
    else:
        if args[0].endswith('prepare-generic-runtime.py') and args[1]=='prepare' and os.environ.get('WINDOWS_SCADA_PATH_ADAPTER')=='1':
            import subprocess
            spec=importlib.util.spec_from_file_location('actual_prepare_stage', args[0])
            p=importlib.util.module_from_spec(spec); spec.loader.exec_module(p)
            original_run=subprocess.run
            def scada_run(command, **kwargs):
                # Git-sh's absolute-root guard needs MSYS syntax; no JSON or
                # SCADA transaction logic is substituted by this path adapter.
                if command[0]=='sh' and command[1].endswith('install-scada-project.sh'):
                    command=list(command)
                    i=command.index('--scada-root')+1
                    converted=original_run(['C:/Program Files/Git/usr/bin/cygpath.exe','-u',command[i]],
                                           capture_output=True,check=True,timeout=10)
                    command[i]=converted.stdout.decode().strip()
                return original_run(command, **kwargs)
            p.subprocess.run=scada_run
            sys.argv=args
            entry=ast.parse(pathlib.Path(args[0]).read_bytes()).body[-1]
            exec(compile(ast.Module(body=entry.body,type_ignores=[]),'<actual-helper-cli>','exec'),p.__dict__)
        else:
            import runpy
            sys.argv=args
            runpy.run_path(args[0],run_name='__main__')
'''


class InstallerPreflightTests(unittest.TestCase):
    def install(self, value, bad='', existing=False, overrides=None, old_mqtt=None, production=False, qt=False, tls=False):
        temp = tempfile.TemporaryDirectory(prefix='kiosk-preflight-local-')
        self.addCleanup(temp.cleanup)
        root = Path(temp.name)
        source = root / 'source'
        home = root / ('target-business-tree-' + 'x' * 60 if production else 'home')
        if tls:
            overrides = dict(overrides or {})
            overrides.update(INIT_TLS_APP_PREPARE='1')
            for field, name in (('CA', 'ca.crt'), ('CERT', 'LOCAL_ONLY_SYNTHETIC-client.pem'), ('KEY', 'LOCAL_ONLY_SYNTHETIC-client.key')):
                overrides.setdefault('INIT_TLS_FINAL_' + field + '_FILE', (home / 'config/runtime/tls' / name).as_posix())
        for name in NAMES:
            write(source / 'deploy' / name, (DEPLOY / name).read_bytes())
        (source / 'config/factory/runtime/apps').mkdir(parents=True)
        (source / 'config/factory/runtime/devices').mkdir()
        for name in ('templates', 'examples'): (source / 'config' / name).mkdir()
        if value is not None:
            write(source / 'config/factory/runtime/apps/monitor-service.json', value if isinstance(value, bytes) else json.dumps(value))
        write(source / 'config/factory/runtime/device_identity.json', '{"machineCode":"LOCAL_ONLY_SYNTHETIC"}')
        for name in ('SystemMonitor', 'MqttDriver', 'MqttForwarder', 'pointctl'):
            write(source / 'bin' / name, b'SYNTHETIC_NOT_EXECUTABLE\n')
        if existing:
            old = support.app(); old['localDisplay']['enabled'] = True
            write(home / 'config/runtime/apps/monitor-service.json', json.dumps(old))
            write(home / 'config/runtime/device_identity.json', '{"machineCode":"OLD_SYNTHETIC"}')
            write(home / 'bin/SystemMonitor', b'OLD_SYNTHETIC_BINARY\n')
            write(home / 'bin/local-kiosk.py', b'OLD_SYNTHETIC_SCRIPT\n')
            write(home / 'kiosk/monitor-service.json', json.dumps(support.helper().kiosk_projection(old)))
            write(home / 'ota/private-fence.synthetic', b'OLD_SYNTHETIC_FENCE\n')
            if old_mqtt is not None:
                write(home / 'config/runtime/apps/mqtt-service.json', json.dumps({'mqtt': old_mqtt}))
        def tree_state():
            return {str(p.relative_to(home)): dict(bytes=p.stat().st_size, sha256=hashlib.sha256(p.read_bytes()).hexdigest())
                    for p in home.rglob('*') if p.is_file()} if home.exists() else {}
        before = tree_state()
        commands = root / 'commands'; commands.mkdir()
        dispatch = root / 'dispatch.py'; write(dispatch, DISPATCH)
        write(commands / 'python3', '#!/bin/sh\nexec "$PYTHON_EXE" -B "$DISPATCH" "$@"\n')
        write(commands / 'systemctl', '#!/bin/sh\nprintf "%s\\n" "$*" >> "$SYSTEMCTL_LOG"\nexit 99\n')
        env = dict(os.environ, PATH=str(commands) + os.pathsep + str(Path(SHELL).parent) + os.pathsep + os.environ.get('PATH',''),
                   PYTHON_EXE=sys.executable, DISPATCH=str(dispatch), MODEL_SOURCE=str(REPO / 'tools/kiosk_public_settings_test.py'),
                   FIXTURE_HOME=str(home), HELPER_TRACE=str(root / 'helper.trace'), BAD_SCRIPT_POLICY=bad,
                   GATEWAY_HOME=home.as_posix(), SOURCE_ROOT=source.as_posix(), DEFAULT_SOURCE_ROOT=source.as_posix(),
                   DEPLOY_DIR=(source / 'deploy').as_posix(), FACTORY_DIR=(source / 'config/factory').as_posix(),
                   TEMPLATES_DIR=(source / 'config/templates').as_posix(), EXAMPLES_DIR=(source / 'config/examples').as_posix(),
                   INSTALL_SYSTEMD='0', START_SERVICES='0', RESET_SHM='0', FACTORY_PROMPT='0', FACTORY_PACKAGE='',
                   TMPDIR=root.as_posix(), FACTORY_TMP_DIR=root.as_posix(), BACKUP_DIR=(home / 'backup').as_posix(),
                   WATCHDOG_RUN_DIR=(root / 'watchdog').as_posix(), SYSTEMD_UNIT_DIR=(root / 'units').as_posix(),
                   SYSTEMCTL_LOG=str(root / 'systemctl.log'), INIT_RUNTIME_MODE='gateway', INIT_LOCAL_QT_DISPLAY_ENABLED='0',
                   INIT_DIRECT_MAINTENANCE_ENABLED='0', INIT_MACHINE_CODE='LOCAL_ONLY_SYNTHETIC',
                   INIT_MQTT_BROKER='tcp://127.0.0.1:1883', INIT_MQTT_USERNAME='', INIT_MQTT_PASSWORD='',
                   INIT_MQTT_TLS_ENABLED='false', INIT_DIRECT_LISTEN_HOSTS='127.0.0.1', INIT_DIRECT_ALLOWED_CIDRS='127.0.0.1/32',
                   OTA_PUBKEY_TARGET=(root / 'never-key').as_posix(), PYTHONDONTWRITEBYTECODE='1')
        for name in tuple(env):
            if name.startswith('INIT_TLS_') or name in ('OTA_SIGNING_PUBKEY', 'INIT_QT_ENV_FILE'):
                env.pop(name)
        for name in ('KIOSK_SERVICE_SOURCE', 'FACTORY_TEST_BASELINE_REF'): env.pop(name, None)
        if overrides:
            env.update(overrides)
        if tls:
            # These are Linux configuration strings, not Windows file inputs.
            env['MSYS2_ARG_CONV_EXCL'] = '/opt/site-config/'
        arguments = [SHELL, str(source / 'deploy/install-factory-config.sh'), '--profile', 'base']
        if production:
            binary_names = ['SystemMonitor', 'MqttDriver', 'MqttForwarder', 'pointctl']
            resources = {}
            if qt:
                write(source / 'build-aarch64/QtDisplayBridge', b'SYNTHETIC_NOT_EXECUTABLE\n')
                write(source / 'ky-ems/KY-EMS', b'SYNTHETIC_NOT_EXECUTABLE\n')
                widgets = (REPO / 'config/generic-scada-widgets.json').read_bytes()
                write(source / 'config/generic-scada-widgets.json', widgets)
                resources['config/generic-scada-widgets.json'] = dict(sha256=hashlib.sha256(widgets).hexdigest(), sizeBytes=len(widgets))
                binary_names += ['QtDisplayBridge', 'KY-EMS']
            binary_paths = {name: ('build-aarch64/QtDisplayBridge' if name == 'QtDisplayBridge' else
                                   'ky-ems/KY-EMS' if name == 'KY-EMS' else 'bin/' + name) for name in binary_names}
            write(source / 'edge-package-manifest.json', json.dumps({'schemaVersion': '1.1', 'profile': 'project' if qt else 'base', 'resources': resources, 'components': [
                {'binary': name, 'path': binary_paths[name], 'version': 'SYNTHETIC1',
                 'sha256': hashlib.sha256((source / binary_paths[name]).read_bytes()).hexdigest(),
                 'sizeBytes': (source / binary_paths[name]).stat().st_size}
                for name in binary_names]}))
            files = {p.relative_to(source).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                     for p in source.rglob('*') if p.is_file()}
            write(source / 'generic-package.json', json.dumps({'files': files}))
            package = root / 'synthetic-factory.tar.gz'
            with tarfile.open(package, 'w:gz') as archive:
                archive.add(source, arcname='gateway-factory-defaults')
            env.update(INIT_WORK_DIR=(root / 'init-work').as_posix(), INIT_START_SERVICES='0',
                       INIT_RUN_SMOKE='0', INIT_PROMPT='0', INIT_KEEP_WORK_DIR='0')
            package_argument = ('/' + package.drive[0].lower() + package.as_posix()[2:]
                                if os.name == 'nt' else str(package))
            arguments = [SHELL, str(source / 'deploy/production-init.sh'), '--auto',
                         '--package', package_argument, '--package-profile', 'project' if qt else 'base',
                         '--local-qt-display' if qt else '--no-local-qt-display']
            if qt:
                arguments += ['--manifest', (source / 'edge-package-manifest.json').as_posix()]
                env['WINDOWS_SCADA_PATH_ADAPTER'] = '1'
            if tls:
                # Production's INSTALL_SYSTEMD=0 guard intentionally forbids
                # enrollment. Exercise the staged installer with the actual Qt
                # preparation, not a simulated successful enrollment branch.
                write(root / 'qt-public-environment.json', '{}')
                env.update(INIT_GENERIC_PREPARE='1', INIT_LOCAL_QT_DISPLAY_ENABLED='1' if qt else '0',
                           INIT_GENERIC_QT_ENVIRONMENT=(root / 'qt-public-environment.json').as_posix())
                arguments = [SHELL, str(source / 'deploy/install-factory-config.sh'), '--profile', 'project' if qt else 'base',
                             '--manifest', (source / 'edge-package-manifest.json').as_posix()]
        result = subprocess.run(arguments,
                                stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env, timeout=40)
        trace_file = root / 'helper.trace'
        trace = trace_file.read_text().splitlines() if trace_file.exists() else []
        self.last_tree_before, self.last_tree_after = before, tree_state()
        print(json.dumps(dict(case=bad or ('missing' if value is None else 'config'), actualInstallerExit=result.returncode,
                              targetExists=home.exists(), helperTrace=trace, stdout=result.stdout.decode(),
                              inputBytes=len(value) if isinstance(value, bytes) else None,
                              targetBefore=before, targetAfter=self.last_tree_after,
                              stderr=result.stderr.decode(), kernelGuardMocked=True, posixMetadataModelled=True)), flush=True)
        self.assertFalse((root / 'systemctl.log').exists())
        return result, home, trace

    def test_optional_missing_monitor(self):
        result, home, _ = self.install(None)
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertFalse((home / 'kiosk/monitor-service.json').exists())

    def test_optional_no_local_display(self):
        result, home, _ = self.install({'systemMonitor': {'enabled': False}})
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertFalse((home / 'kiosk/monitor-service.json').exists())

    def test_disabled_display_ignores_unconsumed_legacy_fields(self):
        for local in ({'enabled': False, 'port': 'legacy', 'kiosk': []},
                      {'enabled': True, 'kiosk': {'enabled': 'false', 'width': 'legacy'}},
                      {'enabled': True, 'renderer': 'nativeQt', 'port': 'legacy'}):
            with self.subTest(local=local):
                result, home, _ = self.install({'localDisplay': local})
                self.assertEqual(0, result.returncode, result.stderr)
                self.assertFalse((home / 'kiosk/monitor-service.json').exists())

    def test_selected_invalid_fails_before_target_mutation(self):
        value = support.app(); value['localDisplay']['enabled'] = True; value['localDisplay']['port'] = True
        result, home, _ = self.install(value)
        self.assertEqual(2, result.returncode)
        self.assertIn(b'KIOSK_SETTINGS_INVALID', result.stderr)
        self.assertFalse(home.exists(), 'selected invalid kiosk discovered only after installed data changed')

    def test_script_policy_failure_precedes_target_mutation(self):
        value = support.app(); value['localDisplay']['enabled'] = True
        for kind in ('owner', 'writable', 'hardlink', 'symlink'):
            with self.subTest(kind=kind):
                result, home, trace = self.install(value, kind)
                self.assertEqual(2, result.returncode)
                self.assertIn(b'KIOSK_IO_FAILURE' if kind == 'symlink' else b'KIOSK_FILE_POLICY', result.stderr)
                self.assertFalse(home.exists(), 'source policy discovered after installed data changed')
                self.assertNotIn('install-kiosk-script', trace)

    def test_selected_valid_preserves_exact_public_values(self):
        value = support.app(); value['localDisplay']['enabled'] = True
        result, home, _ = self.install(value)
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertEqual(support.helper().kiosk_projection(value), json.loads((home / 'kiosk/monitor-service.json').read_bytes()))

    def test_large_original_app_actual_installer_preflight(self):
        for size in (2 * 1024 * 1024, 5 * 1024 * 1024 + 1):
            with self.subTest(size=size):
                raw = support.sized_app(size)
                result, home, _ = self.install(raw)
                if size > 5 * 1024 * 1024:
                    self.assertEqual(2, result.returncode)
                    self.assertIn(b'KIOSK_INPUT_TOO_LARGE', result.stderr)
                    self.assertFalse(home.exists())
                else:
                    self.assertEqual(0, result.returncode, result.stderr)
                    self.assertEqual(support.helper().kiosk_projection(json.loads(raw)),
                                     json.loads((home / 'kiosk/monitor-service.json').read_bytes()))
        fixture = os.environ.get('OTA_M3_PUBLIC_MONITOR_FIXTURE')
        if fixture:
            raw = Path(fixture).read_bytes()
            value = json.loads(raw)
            self.assertEqual(34, len(value['localDisplay']['screens']))
            self.assertGreater(len(raw), 1024 * 1024)
            self.assertLessEqual(len(raw), 5 * 1024 * 1024)
            self.assertFalse(support.helper().kiosk_selected(value))
            result, home, _ = self.install(raw)
            self.assertEqual(0, result.returncode, result.stderr)
            self.assertFalse((home / 'kiosk/monitor-service.json').exists())
            selected = json.loads(raw)
            selected['localDisplay']['renderer'] = 'webkit'
            selected['localDisplay']['kiosk']['enabled'] = True
            selected_raw = json.dumps(selected, ensure_ascii=False, indent=2).encode('utf-8')
            result, home, _ = self.install(selected_raw)
            self.assertEqual(0, result.returncode, result.stderr)
            self.assertEqual(support.helper().kiosk_projection(selected),
                             json.loads((home / 'kiosk/monitor-service.json').read_bytes()))
            installed = json.loads((home / 'config/runtime/apps/monitor-service.json').read_bytes())
            self.assertEqual(value['localDisplay']['screens'], installed['localDisplay']['screens'])
            print(json.dumps(dict(publicDiagnosticFixture=fixture, fullPagesPreserved=34,
                                  originalBytes=len(raw), selectedVariantBytes=len(selected_raw),
                                  selectedVariantOnly='renderer=webkit,kiosk.enabled=true')), flush=True)

    def test_converted_overlimit_preserves_existing_installation(self):
        value = support.app(); value['localDisplay']['enabled'] = True
        value['unknownBusiness'] = [{'index': i, 'value': i % 3} for i in range(10000)]
        value['engineeringOnly'] = ''
        raw = json.dumps(value, separators=(',', ':')).encode('ascii')
        value['engineeringOnly'] = 'x' * (5 * 1024 * 1024 - 1024 - len(raw))
        raw = json.dumps(value, separators=(',', ':')).encode('ascii')
        self.assertLess(len(raw), 5 * 1024 * 1024)
        self.assertGreater(len(json.dumps(value, indent=2).encode('utf-8')), 5 * 1024 * 1024)
        result, home, trace = self.install(raw, existing=True)
        self.assertEqual(2, result.returncode)
        self.assertIn(b'KIOSK_INPUT_TOO_LARGE', result.stderr)
        self.assertEqual(self.last_tree_before, self.last_tree_after,
                         'converted limit discovered only after existing installation changed')
        self.assertNotIn('install-kiosk-script', trace)
        self.assertFalse((home / 'backup').exists())

    def test_staged_conversion_preserves_final_paths_defaults_and_business_data(self):
        value = support.app(); value['localDisplay']['enabled'] = True
        value['unknownBusiness'] = {'points': [{'id': 'SYNTHETIC_POINT', 'extra': [1, 2, 3]}]}
        value['mqtt'] = {'clientId': 'FACTORY_SYNTHETIC', 'broker': 'tcp://127.0.0.1:1884'}
        for explicit in (False, True):
            with self.subTest(explicit=explicit):
                machine = 'OVERRIDE_SYNTHETIC' if explicit else 'OLD_SYNTHETIC'
                broker = 'tcp://127.0.0.1:1886' if explicit else 'tcp://127.0.0.1:1885'
                overrides = {'INIT_MACHINE_CODE': machine if explicit else '',
                             'INIT_MQTT_BROKER': broker if explicit else ''}
                result, home, trace = self.install(value, existing=True, overrides=overrides,
                                                   old_mqtt={'broker': 'tcp://127.0.0.1:1885'})
                self.assertEqual(0, result.returncode, result.stderr)
                raw = (home / 'config/runtime/apps/monitor-service.json').read_bytes()
                installed = json.loads(raw)
                self.assertEqual(value['unknownBusiness'], installed['unknownBusiness'])
                self.assertEqual(value['localDisplay'], installed['localDisplay'])
                self.assertEqual('gateway', installed['runtimeMode'])
                self.assertEqual(machine, installed['mqtt']['clientId'])
                self.assertEqual(broker, installed['mqtt']['broker'])
                self.assertEqual(machine, json.loads((home / 'config/runtime/device_identity.json').read_bytes())['machineCode'])
                direct = installed['systemMonitor']['directMaintenance']
                for field, relative in (
                        ('identityConfigFile', 'config/runtime/device_identity.json'),
                        ('appConfigFile', 'config/runtime/apps/monitor-service.json'),
                        ('otaAppConfigFile', 'config/runtime/apps/mqtt-service.json'),
                        ('authStateFile', 'config/runtime/monitor-direct-maintenance-state.json'),
                        ('otaStatusFile', 'ota/monitor-direct-maintenance-status.jsonl')):
                    self.assertEqual(os.path.join(home.as_posix(), relative), direct[field])
                    self.assertEqual((home / relative).as_posix(), direct[field].replace('\\', '/'))
                self.assertNotIn(b'factory-runtime.', raw)
                self.assertEqual(support.helper().kiosk_projection(value),
                                 json.loads((home / 'kiosk/monitor-service.json').read_bytes()))
                self.assertEqual(b'OLD_SYNTHETIC_FENCE\n', (home / 'ota/private-fence.synthetic').read_bytes())
                self.assertEqual(1, len(list((home / 'backup').glob('runtime-*'))))
                self.assertEqual(2, trace.count('check-kiosk-app'))
                self.assertLess(trace.index('check-kiosk-app', trace.index('check-kiosk-app') + 1),
                                trace.index('prepare-kiosk-parents'))
                self.assertFalse(list(home.parent.glob('factory-runtime.*')))

    def test_production_generic_unicode_overlimit_preserves_existing_installation(self):
        value = support.app(); value['localDisplay']['enabled'] = True
        value['businessNotes'] = '\u5b57' * 900000
        raw = json.dumps(value, ensure_ascii=False, separators=(',', ':')).encode('utf-8')
        self.assertLess(len(raw), 5 * 1024 * 1024)
        self.assertGreater(len(json.dumps(value, ensure_ascii=True, indent=2).encode('utf-8')), 5 * 1024 * 1024)
        result, home, trace = self.install(raw, existing=True, production=True)
        self.assertEqual(2, result.returncode)
        self.assertIn(b'KIOSK_INPUT_TOO_LARGE', result.stderr)
        self.assertEqual(self.last_tree_before, self.last_tree_after)
        self.assertNotIn('install-kiosk-script', trace)
        self.assertFalse((home / 'backup').exists())

    def test_production_staged_unicode_paths_and_syntax(self):
        value = support.app(); value['localDisplay']['enabled'] = True
        value['businessNotes'] = '\u5b57' * 12000
        value['businessDocument'] = '/opt/modbus-gateway/data/business-document.json'
        raw = json.dumps(value, ensure_ascii=False, separators=(',', ':')).encode('utf-8')
        result, home, trace = self.install(raw, existing=True, production=True)
        self.assertEqual(0, result.returncode, result.stderr)
        installed_raw = (home / 'config/runtime/apps/monitor-service.json').read_bytes()
        self.assertLessEqual(len(installed_raw), 5 * 1024 * 1024)
        installed = json.loads(installed_raw)
        self.assertEqual(value['businessNotes'], installed['businessNotes'])
        self.assertEqual(str(home.resolve()) + '/data/business-document.json', installed['businessDocument'])
        self.assertNotIn(b'factory-runtime.', installed_raw)
        self.assertEqual(value['localDisplay'], installed['localDisplay'])
        self.assertEqual(support.helper().kiosk_projection(installed),
                         json.loads((home / 'kiosk/monitor-service.json').read_bytes()))
        self.assertEqual(b'OLD_SYNTHETIC_FENCE\n', (home / 'ota/private-fence.synthetic').read_bytes())
        self.assertEqual(2, trace.count('check-kiosk-app'))
        self.assertFalse(list(home.parent.glob('factory-runtime.*')))
        self.assertFalse((home.parent / 'init-work').exists())
        import ast
        ast.parse((DEPLOY / 'prepare-generic-runtime.py').read_bytes(), feature_version=(3, 6))
        for name in ('production-init.sh', 'install-factory-config.sh'):
            syntax = subprocess.run([SHELL, '-n', str(DEPLOY / name)], capture_output=True, timeout=10)
            self.assertEqual(0, syntax.returncode, syntax.stderr)

    def test_production_qt_stage_real_scada_fields_and_final_size(self):
        for oversized in (False, True):
            with self.subTest(oversized=oversized):
                value = support.app(); value['localDisplay']['enabled'] = True
                value['businessNotes'] = '\u5b57' * (900000 if oversized else 12000)
                if oversized:
                    value['businessDocuments'] = ['/opt/modbus-gateway/data/business-' + str(i) + '.json' for i in range(20000)]
                raw = json.dumps(value, ensure_ascii=False, separators=(',', ':')).encode('utf-8')
                self.assertLess(len(raw), 5 * 1024 * 1024)
                result, home, trace = self.install(raw, existing=True, production=True, qt=True)
                if oversized:
                    self.assertEqual(2, result.returncode, result.stderr)
                    self.assertIn(b'KIOSK_INPUT_TOO_LARGE', result.stderr)
                    self.assertEqual(self.last_tree_before, self.last_tree_after)
                    self.assertNotIn('install-kiosk-script', trace)
                else:
                    self.assertEqual(0, result.returncode, result.stderr)
                    installed_raw = (home / 'config/runtime/apps/monitor-service.json').read_bytes()
                    self.assertLessEqual(len(installed_raw), 5 * 1024 * 1024)
                    installed = json.loads(installed_raw)
                    self.assertEqual(value['businessNotes'], installed['businessNotes'])
                    self.assertEqual(value['localDisplay']['pages'], installed['localDisplay']['pages'])
                    self.assertEqual('nativeQt', installed['localDisplay']['renderer'])
                    self.assertFalse(b'factory-runtime.' in installed_raw, 'private stage path escaped into installed monitor')
                    self.assertEqual((home / 'scada/current').resolve(),
                                     Path(installed['localDisplay']['scada']['projectDirectory']).resolve())
                    self.assertTrue((home / 'scada/current/manifest.json').is_file())

    def test_tls_final_expansion_rejected_before_existing_tree_changes(self):
        value = json.loads(support.sized_app(5 * 1024 * 1024 - 4096))
        value['mqtt'] = {'broker': 'tcp://127.0.0.1:1883'}
        raw = json.dumps(value, separators=(',', ':')).encode('ascii')
        destination = '/opt/site-config/' + '/'.join('business-area-' + str(i) for i in range(140))
        ca, cert, key = (destination + '/' + name for name in ('ca.crt', 'client.pem', 'client.key'))
        self.assertTrue(all(len(path) < 4096 for path in (ca, cert, key)))
        result, home, trace = self.install(raw, existing=True, overrides={
            'INIT_TLS_APP_PREPARE': '1', 'INIT_TLS_FINAL_CA_FILE': ca,
            'INIT_TLS_FINAL_CERT_FILE': cert, 'INIT_TLS_FINAL_KEY_FILE': key})
        if result.returncode == 0:
            # Only the actual post-enrollment configuration block executes;
            # no certificate, private key, CSR, token, crypto or network code.
            source = (DEPLOY / 'gateway-tls-enroll.sh').read_text()
            inline = source.split("<<'PY'\n")[-1].split('\nPY\n')[0]
            tree = ast.parse(inline)
            update = next(node for node in tree.body if isinstance(node, ast.If)
                          and ast.unparse(node.test) == "update_local_app == '1'")
            namespace = dict(update_local_app='1', app_config=str(home / 'config/runtime/apps/mqtt-service.json'),
                             ca_file=ca, cert_file=cert, key_file=key, glob=glob, os=os, json=json)
            exec(compile(ast.Module(body=[update], type_ignores=[]), '<actual-tls-app-update>', 'exec'), namespace)
            output = (home / 'config/runtime/apps/monitor-service.json').read_bytes()
            print(json.dumps(dict(actualTlsConfigOnly=True, tlsScriptSha256=hashlib.sha256(source.encode()).hexdigest(),
                                  finalMonitorBytes=len(output), finalMonitorSha256=hashlib.sha256(output).hexdigest(),
                                  noEnrollmentOrIdentityGenerated=True)), flush=True)
        self.assertEqual(2, result.returncode)
        self.assertIn(b'KIOSK_INPUT_TOO_LARGE', result.stderr)
        self.assertEqual(self.last_tree_before, self.last_tree_after)
        self.assertNotIn('install-kiosk-script', trace)


    def test_tls_staged_bytes_match_real_provider_config_function(self):
        for custom, qt in ((False, False), (True, True)):
            with self.subTest(custom=custom, qt=qt):
                value = support.app(); value['localDisplay']['enabled'] = True
                value['mqtt'] = {'broker': 'tcp://127.0.0.1:1883', 'tls': {'businessPolicy': ['retain', 9]}}
                value['businessNotes'] = '\u5b57' * 12000
                overrides = {'INIT_TLS_FINAL_' + field + '_FILE': '/opt/site-config/' + name
                             for field, name in (('CA', 'custom-ca.crt'), ('CERT', 'custom-client.pem'), ('KEY', 'custom-client.key'))} if custom else {}
                result, home, trace = self.install(value, existing=True, production=qt, qt=qt, tls=True, overrides=overrides)
                self.assertEqual(0, result.returncode, result.stderr)
                apps = home / 'config/runtime/apps'
                before = {p.name: p.read_bytes() for p in apps.glob('*.json')}
                installed = json.loads(before['monitor-service.json'])
                ca, cert, key = [overrides.get('INIT_TLS_FINAL_' + field + '_FILE',
                                               (home / 'config/runtime/tls' / name).as_posix())
                                 for field, name in (('CA', 'ca.crt'), ('CERT', 'LOCAL_ONLY_SYNTHETIC-client.pem'), ('KEY', 'LOCAL_ONLY_SYNTHETIC-client.key'))]
                self.assertEqual(dict(businessPolicy=['retain', 9], enabled=True, caFile=ca, certFile=cert,
                                      keyFile=key, insecureSkipVerify=False), installed['mqtt']['tls'])
                self.assertEqual(value['businessNotes'], installed['businessNotes'])
                self.assertEqual(value['localDisplay']['pages'], installed['localDisplay']['pages'])
                self.assertNotIn(b'factory-runtime.', before['monitor-service.json'])
                self.assertIn('prepare-tls-apps', trace)
                # Execute the actual provider's config-only shell function. The
                # enclosing enrollment script/openssl/network path never runs.
                source = (DEPLOY / 'gateway-tls-enroll.sh').read_text()
                function = 'update_tls_apps() {' + source.split('update_tls_apps() {', 1)[1].split('\n}\n', 1)[0] + '\n}\n'
                env = dict(os.environ, SCRIPT_DIR=(home.parent / 'source/deploy').as_posix(),
                           APP_CONFIG=(apps / 'mqtt-service.json').as_posix(), CA_FILE=ca, CERT_FILE=cert, KEY_FILE=key,
                           REQUIRE_PREPARED_APP='1', FIXTURE_HOME=home.as_posix(), HELPER_TRACE=(home.parent / 'helper.trace').as_posix(),
                           MODEL_SOURCE=str(REPO / 'tools/kiosk_public_settings_test.py'),
                           PYTHON_EXE=sys.executable, DISPATCH=str(home.parent / 'dispatch.py'), PYTHONDONTWRITEBYTECODE='1',
                           MSYS2_ARG_CONV_EXCL='/opt/site-config/')
                # The existing fixture bin contains the Python dispatch adapter.
                env['PATH'] = str(home.parent / 'commands') + os.pathsep + str(Path(SHELL).parent) + os.pathsep + env['PATH']
                provider = subprocess.run([SHELL, '-c', 'set -eu\n' + function + '\nupdate_tls_apps'],
                                          env=env, capture_output=True, timeout=15)
                self.assertEqual(0, provider.returncode, provider.stderr)
                self.assertEqual(before, {p.name: p.read_bytes() for p in apps.glob('*.json')})
                changed = json.loads(before['monitor-service.json']); changed['mqtt']['tls']['enabled'] = False
                (apps / 'monitor-service.json').write_bytes(json.dumps(changed).encode())
                changed_before = {p.name: p.read_bytes() for p in apps.glob('*.json')}
                rejected = subprocess.run([SHELL, '-c', 'set -eu\n' + function + '\nupdate_tls_apps'],
                                          env=env, capture_output=True, timeout=15)
                self.assertEqual(2, rejected.returncode, rejected.stderr)
                self.assertIn(b'TLS_APP_NOT_PREPARED', rejected.stderr)
                self.assertEqual(changed_before, {p.name: p.read_bytes() for p in apps.glob('*.json')})
                print(json.dumps(dict(tlsConfigOnly=True, custom=custom, qt=qt, providerExit=provider.returncode,
                                      driftExit=rejected.returncode, noEnrollmentOrIdentityGenerated=True,
                                      monitorSha256=hashlib.sha256(before['monitor-service.json']).hexdigest())), flush=True)

    def test_tls_production_selection_and_provider_default_custom_paths(self):
        source = (DEPLOY / 'production-init.sh').read_text()
        selection = 'export INIT_TLS_APP_PREPARE=0\n' + source.split('export INIT_TLS_APP_PREPARE=0\n', 1)[1].split('export INIT_GENERIC_PREPARE=0', 1)[0]
        truthy = 'truthy() {' + source.split('truthy() {', 1)[1].split('\n}\n', 1)[0] + '\n}\n'
        for online, update, signed, expected in ((True, '1', False, '1'), (True, 'false', False, '0'),
                                                (False, '1', False, '0'), (True, '1', True, '0')):
            env = dict(os.environ, tls_requested='1', INIT_TLS_UPDATE_LOCAL_APP=update,
                       PATH=str(Path(SHELL).parent) + os.pathsep + os.environ.get('PATH', ''),
                       INIT_TLS_PLATFORM_URL='https://public.invalid' if online else '',
                       INIT_TLS_ENROLLMENT_TOKEN='SYNTHETIC_NO_AUTHORITY' if online else '',
                       INIT_TLS_CA_SOURCE='/synthetic/ca' if signed else '', INIT_TLS_CERT_SOURCE='', INIT_TLS_KEY_SOURCE='',
                       INIT_MQTT_CA_FILE='/synthetic/ca.crt', INIT_MQTT_CERT_FILE='/synthetic/client.pem', INIT_MQTT_KEY_FILE='/synthetic/client.key')
            result = subprocess.run([SHELL, '-c', 'set -eu\n' + truthy + selection + '\nprintf "%s" "$INIT_TLS_APP_PREPARE"'],
                                    env=env, capture_output=True, timeout=10)
            self.assertEqual(0, result.returncode, result.stderr)
            self.assertEqual(expected, result.stdout.decode())
        enroll = (DEPLOY / 'gateway-tls-enroll.sh').read_text()
        assignments = '\n'.join(line for line in enroll.splitlines() if line.startswith(('KEY_FILE="${KEY_FILE:-', 'CERT_FILE="${CERT_FILE:-', 'CA_FILE="${CA_FILE:-')))
        self.assertEqual(3, len(assignments.splitlines()))
        for custom in (False, True):
            env = dict(os.environ, TLS_DIR='/synthetic/runtime/tls', SAFE_MACHINE='PUBLIC_MACHINE',
                       PATH=str(Path(SHELL).parent) + os.pathsep + os.environ.get('PATH', ''),
                       CA_FILE='/synthetic/custom/ca' if custom else '', CERT_FILE='/synthetic/custom/cert' if custom else '',
                       KEY_FILE='/synthetic/custom/key' if custom else '')
            result = subprocess.run([SHELL, '-c', 'set -eu\n' + assignments + '\nprintf "%s\\n" "$CA_FILE" "$CERT_FILE" "$KEY_FILE"'],
                                    env=env, capture_output=True, timeout=10)
            self.assertEqual(0, result.returncode, result.stderr)
            self.assertEqual(['/synthetic/custom/' + name for name in ('ca', 'cert', 'key')] if custom else
                             ['/synthetic/runtime/tls/' + name for name in ('ca.crt', 'PUBLIC_MACHINE-client.pem', 'PUBLIC_MACHINE-client.key')],
                             result.stdout.decode().splitlines())
        ast.parse((DEPLOY / 'prepare-generic-runtime.py').read_bytes(), feature_version=(3, 6))
        for name in ('production-init.sh', 'install-factory-config.sh', 'gateway-tls-enroll.sh'):
            syntax = subprocess.run([SHELL, '-n', str(DEPLOY / name)], capture_output=True, timeout=10)
            self.assertEqual(0, syntax.returncode, syntax.stderr)


class StandaloneTlsPublicationTests(unittest.TestCase):
    def enrollment(self, mode, required=False, pre_drift=False, url='https://public.invalid', missing_ca=False):
        temp = tempfile.TemporaryDirectory(prefix='tls-publication-local-')
        self.addCleanup(temp.cleanup)
        root = Path(temp.name)
        home = root / 'home'
        apps = home / 'config/runtime/apps'
        tls = home / 'config/runtime/tls'
        source = root / 'deploy'
        for name in ('gateway-tls-enroll.sh', 'prepare-generic-runtime.py'):
            write(source / name, (DEPLOY / name).read_bytes())
        paths = [str(tls / name).replace('\\', '/') for name in ('ca.crt', 'SYNTHETIC-client.pem', 'SYNTHETIC-client.key')]
        values = {'aaa.json': {'mqtt': {}, 'business': [1, 'retain']},
                  'mqtt-service.json': {'mqtt': {'tls': {'sitePolicy': 'retain'}}},
                  'monitor-service.json': support.app()}
        values['monitor-service.json']['mqtt'] = {'tls': {'sitePolicy': 'retain'}}
        for name, value in values.items():
            raw = json.dumps(value).encode()
            if required:
                raw = support.helper().tls_app_bytes(raw, *paths, monitor=name == 'monitor-service.json')
            write(apps / name, raw)
        if pre_drift:
            value = json.loads((apps / 'monitor-service.json').read_bytes())
            value['mqtt']['tls']['enabled'] = False
            write(apps / 'monitor-service.json', json.dumps(value))
        def app_hashes():
            return {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(apps.glob('*.json'))}
        before = app_hashes()
        commands = root / 'commands'
        write(root / 'dispatch.py', DISPATCH)
        write(commands / 'python3', '#!/bin/sh\nexec "$PYTHON_EXE" -B "$DISPATCH" "$@"\n')
        write(commands / 'openssl', '''#!/bin/sh
printf '%s\\n' 'PUBLIC_STUB_NO_CRYPTO' >> "$TLS_OPENSSL_TRACE"
while [ "$#" -gt 0 ]; do
  if [ "$1" = '-out' ]; then
    case "$2" in "$TLS_FIXTURE_ROOT"/*) ;; *) exit 98 ;; esac
    printf '%s\\n' 'PUBLIC_STUB_NOT_A_KEY_OR_CSR' > "$2"
    exit 0
  fi
  shift
done
exit 97
''')
        write(commands / 'systemctl', '#!/bin/sh\nexit 99\n')
        env = {name: os.environ[name] for name in ('SystemRoot', 'WINDIR') if name in os.environ}
        env.update(PATH=str(commands) + os.pathsep + str(Path(SHELL).parent),
                   PYTHON_EXE=sys.executable, DISPATCH=str(root / 'dispatch.py'),
                   MODEL_SOURCE=str(REPO / 'tools/kiosk_public_settings_test.py'),
                   FIXTURE_HOME=str(home), GATEWAY_HOME=home.as_posix(),
                   HELPER_TRACE=str(root / 'helper.trace'), TLS_CONFIG_TRACE=str(root / 'config.trace'),
                   TLS_ENROLLMENT_FIXTURE=mode, TLS_HTTP_TRACE=str(root / 'http.trace'),
                   TLS_OPENSSL_TRACE=str(root / 'openssl.trace'), TLS_DRIFT_TRACE=str(root / 'drift.trace'),
                   TLS_FIXTURE_ROOT=tls.as_posix(), PYTHONDONTWRITEBYTECODE='1')
        args = [SHELL, str(source / 'gateway-tls-enroll.sh'), '--machine-code', 'SYNTHETIC',
                '--platform-url', url, '--token', 'SYNTHETIC_NO_AUTHORITY', '--update-local-app']
        if required:
            args.append('--require-prepared-app')
        if missing_ca:
            args += ['--platform-ca', (root / 'missing-public-ca').as_posix()]
        result = subprocess.run(args, env=env, stdin=subprocess.DEVNULL, capture_output=True, timeout=20)
        traces = [json.loads(line) for line in (root / 'config.trace').read_text().splitlines()]
        print(json.dumps(dict(case=mode, required=required, preDrift=pre_drift, url=url, missingCa=missing_ca,
                              actualExit=result.returncode, before=before, after=app_hashes(), trace=traces,
                              stdout=result.stdout.decode(), stderr=result.stderr.decode(),
                              opensslStub=(root / 'openssl.trace').read_text() if (root / 'openssl.trace').exists() else '',
                              httpStub=(root / 'http.trace').read_text() if (root / 'http.trace').exists() else '',
                              posixMetadataModelled=True, realCryptoOrNetwork=False)), flush=True)
        return result, root, apps, tls, before, app_hashes(), traces, values, paths

    def test_enrollment_failure_preserves_app_bytes(self):
        cases = (('http-error', 'http://public.invalid', False, 2, 'refusing http://'),
                 ('http-error', 'https://public.invalid', True, 2, 'platform CA file is not readable'),
                 ('http-error', 'https://public.invalid', False, 1, 'platform sign request failed: HTTP 403: SYNTHETIC_DENIAL'))
        for mode, url, missing, code, reason in cases:
            with self.subTest(url=url, missingCa=missing):
                result, root, apps, tls, before, after, trace, _, _ = self.enrollment(mode, url=url, missing_ca=missing)
                self.assertEqual(code, result.returncode, result.stderr)
                self.assertIn(reason, result.stderr.decode())
                self.assertFalse((tls / 'SYNTHETIC-client.pem').exists())
                self.assertEqual(before, after)
                self.assertFalse((root / 'http.trace').exists()) if code == 2 else self.assertEqual('http-error\n', (root / 'http.trace').read_text())

    def test_success_publishes_only_after_certificate_writes(self):
        result, _, apps, tls, before, after, trace, original, paths = self.enrollment('success')
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertEqual(['before', 'after', 'before', 'after'], [t['phase'] for t in trace])
        self.assertIn('--check-only', trace[0]['flags'])
        self.assertNotIn('--check-only', trace[2]['flags'])
        self.assertEqual(before, trace[1]['apps'])
        self.assertFalse(trace[0]['certExists'] or trace[0]['caExists'])
        self.assertTrue(trace[2]['certExists'] and trace[2]['caExists'])
        self.assertNotEqual(before, after)
        for name, old in original.items():
            expected = json.loads(json.dumps(old))
            expected['mqtt'].setdefault('tls', {}).update(enabled=True, caFile=paths[0], certFile=paths[1], keyFile=paths[2], insecureSkipVerify=False)
            self.assertEqual(expected, json.loads((apps / name).read_bytes()))
        self.assertIn('PUBLIC_STUB_NOT_A_CERTIFICATE', (tls / 'SYNTHETIC-client.pem').read_text())

    def test_required_pre_and_post_drift_reject_without_rewriting(self):
        for pre in (True, False):
            with self.subTest(preDrift=pre):
                result, root, _, tls, before, after, trace, _, _ = self.enrollment('post-drift', required=True, pre_drift=pre)
                self.assertEqual(2, result.returncode, result.stderr)
                self.assertIn('TLS_APP_NOT_PREPARED', result.stderr.decode())
                self.assertTrue(all('--require-prepared' in t['flags'] for t in trace))
                if pre:
                    self.assertEqual(before, after)
                    self.assertFalse((root / 'openssl.trace').exists())
                    self.assertFalse((root / 'http.trace').exists())
                    self.assertFalse((tls / 'SYNTHETIC-client.pem').exists())
                else:
                    expected = dict(before, **{'monitor-service.json': (root / 'drift.trace').read_text()})
                    self.assertEqual(expected, after)
                    self.assertTrue(trace[-1]['certExists'] and trace[-1]['caExists'])
                    self.assertEqual(before, trace[1]['apps'])


if __name__ == '__main__': unittest.main(verbosity=2)
