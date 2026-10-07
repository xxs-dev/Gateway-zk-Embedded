"""Actual private installer shell; Windows POSIX owner/syscall model, not Linux DAC."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
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
         'offline-runtime-upgrade.py', 'production_role_policy.py')


def write(path, raw):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(raw.encode('utf-8') if isinstance(raw, str) else raw)


DISPATCH = r'''import ast, importlib.util, json, os, pathlib, stat, sys
sys.stdout.reconfigure(newline='\n')
args=sys.argv[1:]
if args and args[0].endswith('runtime-upgrade-guard.py'):
    assert args[1]=='factory'
    print('FIXTURE_KERNEL_GUARD_NOT_EXECUTED')
    sys.exit(0)
if args and args[0].endswith('prepare-generic-runtime.py'):
    spec=importlib.util.spec_from_file_location('test_model', os.environ['MODEL_SOURCE'])
    model=importlib.util.module_from_spec(spec); spec.loader.exec_module(model)
    spec=importlib.util.spec_from_file_location('actual_prepare', args[0])
    p=importlib.util.module_from_spec(spec); spec.loader.exec_module(p)
    fs=model.PosixModel()
    home=pathlib.Path(os.environ['FIXTURE_HOME'])
    mapped=list(args[1:])
    if '--home' in mapped:
        mapped[mapped.index('--home')+1]='/opt/gateway'
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
    entry=ast.parse(pathlib.Path(args[0]).read_bytes()).body[-1]
    assert isinstance(entry,ast.If)
    exec(compile(ast.Module(body=entry.body,type_ignores=[]),'<actual-helper-cli>', 'exec'),p.__dict__)
    if mapped[0]=='prepare-kiosk-parents':
        (home/'bin').mkdir(parents=True,exist_ok=True); (home/'kiosk').mkdir(exist_ok=True)
    if mapped[0]=='install-kiosk-script':
        (home/'bin/local-kiosk.py').write_bytes(fs.entries['/opt/gateway/bin/local-kiosk.py']['data'])
    if mapped[0]=='project-kiosk' and '/opt/gateway/kiosk/monitor-service.json' in fs.entries:
        old.write_bytes(fs.entries['/opt/gateway/kiosk/monitor-service.json']['data'])
else:
    if args and args[0]=='-':
        namespace={'__name__':'__main__'}
        sys.argv=['-']+args[1:]
        exec(compile(sys.stdin.buffer.read(),'<actual-installer-inline-python>','exec'),namespace)
    else:
        import runpy
        sys.argv=args
        runpy.run_path(args[0],run_name='__main__')
'''


class InstallerPreflightTests(unittest.TestCase):
    def install(self, value, bad=''):
        temp = tempfile.TemporaryDirectory(prefix='kiosk-preflight-local-')
        self.addCleanup(temp.cleanup)
        root = Path(temp.name)
        source, home = root / 'source', root / 'home'
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
        for name in ('KIOSK_SERVICE_SOURCE', 'FACTORY_TEST_BASELINE_REF'): env.pop(name, None)
        result = subprocess.run([SHELL, str(source / 'deploy/install-factory-config.sh'), '--profile', 'base'],
                                stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env, timeout=40)
        trace = (root / 'helper.trace').read_text().splitlines()
        print(json.dumps(dict(case=bad or ('missing' if value is None else 'config'), actualInstallerExit=result.returncode,
                              targetExists=home.exists(), helperTrace=trace, stdout=result.stdout.decode(),
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


if __name__ == '__main__': unittest.main(verbosity=2)
