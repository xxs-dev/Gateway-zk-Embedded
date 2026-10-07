"""Local resource tests; synthetic identity only, never execute target binaries."""
import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
from types import SimpleNamespace
import unittest
import zipfile

SOURCE = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('generic_runtime', SOURCE / 'deploy/prepare-generic-runtime.py')
RUNTIME = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RUNTIME)


def write(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2) + '\n', encoding='utf-8')


class GenericQtResourceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='qt-resource-local-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.package = self.root / 'package'
        self.home = self.root / 'old-home'
        candidate = os.environ.get('GW_QT_FACTORY_CANDIDATE')
        if candidate:
            with tarfile.open(candidate, 'r:gz') as archive:
                for entry in archive:
                    if not entry.isfile():
                        continue
                    name = entry.name.removeprefix('gateway-factory-defaults/')
                    if name == entry.name or '..' in Path(name).parts or Path(name).is_absolute():
                        raise ValueError('unsafe test candidate member')
                    if entry.size > 32 * 1024 * 1024:
                        raise ValueError('test candidate member bound')
                    target = self.package / name
                    target.parent.mkdir(parents=True, exist_ok=True)
                    with archive.extractfile(entry) as stream:
                        target.write_bytes(stream.read(entry.size + 1))
        else:
            write(self.package / 'edge-package-manifest.json', {
                'components': [{'binary': 'KY-EMS'}, {'binary': 'QtDisplayBridge'}]})
            write(self.package / 'generic-package.json', {'files': {}})
        write(self.home / 'config/runtime/apps/monitor-service.json', {'oldProtectedConfig': True})
        # A leftover file in home must never satisfy a missing package resource.
        write(self.home / 'config/generic-scada-widgets.json', {'residual': 'not-package-input'})
        self.fence = self.home / 'run/protected-fence'
        self.fence.parent.mkdir(parents=True)
        self.fence.write_bytes(b'SYNTHETIC-PROTECTED-STATE\n')
        self.before = {path.relative_to(self.home).as_posix(): path.read_bytes()
                       for path in self.home.rglob('*') if path.is_file()}
        self.manifest = self.root / 'selection-input.json'
        write(self.manifest, {'requiredDrivers': ['KY-EMS']})
        self.args = SimpleNamespace(package_root=self.package, deploy_dir=self.package / 'deploy',
            mode='gateway', machine='SYNTHETIC_QT_001', profile='project', manifest=str(self.manifest),
            qt='1', home=self.home, start='0', selection=self.root / 'selection-output.json')

    def assert_old_state(self):
        self.assertEqual(self.before, {path.relative_to(self.home).as_posix(): path.read_bytes()
                                      for path in self.home.rglob('*') if path.is_file()})
        self.assertFalse(self.args.selection.exists())

    def test_missing_package_widgets_reject_before_mutation_despite_home_residual(self):
        self.assertFalse((self.package / 'config/generic-scada-widgets.json').exists())
        with self.assertRaisesRegex(ValueError, 'QT_WIDGET_RESOURCE'):
            RUNTIME.preflight(self.args)
        self.assert_old_state()

    def add_bound_widgets(self, widgets=None):
        resource = self.package / RUNTIME.QT_WIDGET_RESOURCE
        resource.parent.mkdir(parents=True, exist_ok=True)
        if widgets is None:
            resource.write_bytes((SOURCE / RUNTIME.QT_WIDGET_RESOURCE).read_bytes())
        else:
            write(resource, widgets)
        manifest = RUNTIME.read(self.package / 'edge-package-manifest.json')
        manifest.setdefault('resources', {})[RUNTIME.QT_WIDGET_RESOURCE] = {
            'sha256': RUNTIME.sha(resource), 'sizeBytes': resource.stat().st_size}
        write(self.package / 'edge-package-manifest.json', manifest)
        generic = RUNTIME.read(self.package / 'generic-package.json')
        generic['files'].update({RUNTIME.QT_WIDGET_RESOURCE: RUNTIME.sha(resource),
                                'edge-package-manifest.json': RUNTIME.sha(self.package / 'edge-package-manifest.json')})
        write(self.package / 'generic-package.json', generic)

    def test_resource_is_exact_product_projection_without_site_binding(self):
        widgets = RUNTIME.read(SOURCE / RUNTIME.QT_WIDGET_RESOURCE)
        source = SOURCE / widgets['_source']['path']
        raw = source.read_bytes().replace(b'\r\n', b'\n')
        self.assertEqual(widgets['_source']['sha256Lf'], hashlib.sha256(raw).hexdigest())
        product = json.loads(raw)
        for kind in ('qtLabel', 'qtValue', 'qtButton'):
            expected = copy.deepcopy(next(item for item in product['widgets'] if item['type'] == kind))
            expected.update(widgetId='generic', title='', styleClass='', bindings=[], stateRules=[], action=None)
            expected['properties'] = {'qtText': '', 'qtTextColor': '#17212B', 'qtBackgroundColor': 'transparent',
                'qtFontSize': 18, 'qtTextAlignment': 'Left', 'qtTransparent': True}
            self.assertEqual(expected, widgets[kind])

    def test_bound_resource_installs_exact_bytes_and_generates_synthetic_routes_screens(self):
        self.add_bound_widgets()
        RUNTIME.preflight(self.args)
        command = [sys.executable, '-B', str(SOURCE / 'deploy/prepare-generic-runtime.py'),
                   'install-widgets', '--package-root', str(self.package), '--home', str(self.home)]
        result = subprocess.run(command, capture_output=True, timeout=30)
        self.assertEqual(0, result.returncode, result.stderr.decode())
        self.assertEqual((self.package / RUNTIME.QT_WIDGET_RESOURCE).read_bytes(),
                         (self.home / RUNTIME.QT_WIDGET_RESOURCE).read_bytes())
        device = self.home / 'config/runtime/devices/synthetic-only.json'
        points = [{'index': i + 1, 'pointCode': 'P' + str(i), 'name': 'Synthetic ' + str(i), 'unit': 'kW'}
                  for i in range(18)]
        write(device, {'enabled': True, 'memoryStore': {'sharedMemoryName': 'synthetic-only-store'},
                       'meters': [{'meterCode': 'SYNTHETIC_METER_ONLY', 'points': points}]})
        write(self.home / 'config/runtime/apps/monitor-service.json', {'deviceConfigFiles': [str(device)]})
        output = self.root / 'synthetic-runtime.kyscada'
        RUNTIME.make_project(self.home, self.args.machine, output)
        with zipfile.ZipFile(output) as archive:
            tags = json.loads(archive.read('tags.json'))
            routes = json.loads(archive.read('runtime-map.json'))
            nodes = json.loads(archive.read('nodes.json'))
            self.assertEqual(self.args.machine, nodes[0]['machineCode'])
            self.assertEqual(18, len(tags))
            self.assertEqual(18, len(routes))
            self.assertEqual({p['index'] for p in points}, {route['index'] for route in routes})
            self.assertEqual({'SYNTHETIC_METER_ONLY'}, {tag['meterCode'] for tag in tags})
            self.assertEqual({'synthetic-only-store'}, {route['sharedMemoryName'] for route in routes})
            self.assertTrue(all(route['writable'] is False for route in routes))
            screens = [json.loads(archive.read(name)) for name in archive.namelist() if name.startswith('screens/')]
            self.assertEqual({'Points1', 'Points2'}, {screen['screenId'] for screen in screens})
            bindings = [binding['tagId'] for screen in screens for widget in screen['widgets'] for binding in widget['bindings']]
            self.assertEqual({tag['tagId'] for tag in tags}, set(bindings))
            checksums = json.loads(archive.read('checksums.json'))
            for name, digest in checksums.items():
                self.assertEqual(digest, hashlib.sha256(archive.read(name)).hexdigest())
        for invalid in ('conflict', 'missing-identity'):
            with self.subTest(invalid=invalid):
                altered = copy.deepcopy(points)
                if invalid == 'conflict':
                    altered[1]['index'] = altered[0]['index']
                else:
                    altered[0]['pointCode'] = ''
                write(device, {'memoryStore': {'sharedMemoryName': 'synthetic-only-store'},
                               'meters': [{'meterCode': 'SYNTHETIC_METER_ONLY', 'points': altered}]})
                rejected = self.root / (invalid + '.kyscada')
                with self.assertRaisesRegex(ValueError, 'ambiguous runtime point|incomplete runtime point'):
                    RUNTIME.make_project(self.home, self.args.machine, rejected)
                self.assertFalse(rejected.exists())

    def test_unbound_mismatch_and_unusable_prototypes_reject_without_mutation(self):
        original = RUNTIME.read(SOURCE / RUNTIME.QT_WIDGET_RESOURCE)
        for defect in ('unbound', 'manifest-mismatch', 'hash-mismatch', 'missing-type', 'bad-properties', 'site-binding'):
            with self.subTest(defect=defect):
                broken = copy.deepcopy(original)
                if defect == 'missing-type':
                    del broken['qtButton']
                elif defect == 'bad-properties':
                    broken['qtValue']['properties'] = None
                elif defect == 'site-binding':
                    broken['qtValue']['bindings'] = [{'tagId': 'not-a-prototype'}]
                self.add_bound_widgets(broken)
                generic = RUNTIME.read(self.package / 'generic-package.json')
                if defect == 'unbound':
                    del generic['files'][RUNTIME.QT_WIDGET_RESOURCE]
                elif defect == 'manifest-mismatch':
                    manifest = RUNTIME.read(self.package / 'edge-package-manifest.json')
                    manifest['resources'][RUNTIME.QT_WIDGET_RESOURCE]['sizeBytes'] += 1
                    write(self.package / 'edge-package-manifest.json', manifest)
                    generic['files']['edge-package-manifest.json'] = RUNTIME.sha(self.package / 'edge-package-manifest.json')
                elif defect == 'hash-mismatch':
                    generic['files'][RUNTIME.QT_WIDGET_RESOURCE] = '0' * 64
                write(self.package / 'generic-package.json', generic)
                with self.assertRaisesRegex(ValueError, 'QT_WIDGET_RESOURCE|bundle hash mismatch'):
                    RUNTIME.preflight(self.args)
                self.assert_old_state()

    def test_qt_disabled_keeps_existing_valid_path(self):
        self.args.qt = '0'
        RUNTIME.preflight(self.args)
        self.assertTrue(self.args.selection.exists())
        self.assertEqual(self.before, {path.relative_to(self.home).as_posix(): path.read_bytes()
                                      for path in self.home.rglob('*') if path.is_file()})

    def test_packager_manifest_program_binds_copied_resource_bytes(self):
        self.add_bound_widgets()
        script = (SOURCE / 'deploy/build-factory-package.sh').read_text(encoding='utf-8')
        function = script.split('finalize_package_manifest() {\n', 1)[1].split('\nfactory_requires_ems_runtime()', 1)[0]
        program = function.split("<<'PY'\n", 1)[1].split('\nPY\n', 1)[0]
        manifest_path = self.package / 'edge-package-manifest.json'
        original = RUNTIME.read(manifest_path)
        if not os.environ.get('GW_QT_FACTORY_CANDIDATE'):
            for item in original['components']:
                relative = 'ky-ems/KY-EMS' if item['binary'] == 'KY-EMS' else 'build-aarch64/' + item['binary']
                path = self.package / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b'synthetic manifest input, never execute')
                item.update(sha256=RUNTIME.sha(path), sizeBytes=path.stat().st_size)
            write(manifest_path, original)
        packaged = ' '.join(item['binary'] for item in original['components'])
        # Only the actual embedded manifest program runs; no shell installer or ELF.
        result = subprocess.run([sys.executable, '-B', '-c', program, str(manifest_path),
            'full', str(self.package), packaged, packaged, 'a' * 40, 'false',
            'synthetic-manifest-only', 'retained-product03-not-compiled'], capture_output=True, timeout=30)
        self.assertEqual(0, result.returncode, result.stderr.decode())
        manifest = RUNTIME.read(manifest_path)
        resource = self.package / RUNTIME.QT_WIDGET_RESOURCE
        self.assertEqual({'sha256': RUNTIME.sha(resource), 'sizeBytes': resource.stat().st_size},
                         manifest['resources'][RUNTIME.QT_WIDGET_RESOURCE])
        self.assertEqual({item['binary']: (item['sha256'], item['sizeBytes']) for item in original['components']},
                         {item['binary']: (item['sha256'], item['sizeBytes']) for item in manifest['components']})

    def test_install_rechecks_package_and_preserves_old_home_on_changed_resource(self):
        self.add_bound_widgets()
        RUNTIME.preflight(self.args)
        resource = self.package / RUNTIME.QT_WIDGET_RESOURCE
        resource.write_bytes(resource.read_bytes() + b' ')
        before = {path.relative_to(self.home).as_posix(): path.read_bytes()
                  for path in self.home.rglob('*') if path.is_file()}
        result = subprocess.run([sys.executable, '-B', str(SOURCE / 'deploy/prepare-generic-runtime.py'),
            'install-widgets', '--package-root', str(self.package), '--home', str(self.home)],
            capture_output=True, timeout=30)
        self.assertEqual(2, result.returncode)
        self.assertIn(b'QT_WIDGET_RESOURCE_MISMATCH', result.stderr)
        self.assertEqual(before, {path.relative_to(self.home).as_posix(): path.read_bytes()
                                 for path in self.home.rglob('*') if path.is_file()})


if __name__ == '__main__':
    unittest.main(verbosity=2)
