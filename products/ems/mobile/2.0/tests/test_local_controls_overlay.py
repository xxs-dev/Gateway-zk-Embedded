import copy
import importlib.util
import json
import os
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[5]
spec = importlib.util.spec_from_file_location('overlay', ROOT / 'products/ems/mobile/2.0/tools/overlay-local-controls.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
SOURCE = ROOT / 'artifacts/comm104-deploy-20260912/snapshot-095831/running/scada/current'
CONTROLS = ROOT / 'tools/test-output/comm104-ui-final-r3/scada-project'


class OverlayTest(unittest.TestCase):
    def test_live_project_is_preserved(self):
        before = module.hashes(SOURCE)
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp) / 'candidate'
            report = module.overlay(SOURCE, CONTROLS, output)
            project = output / 'scada-project'
            self.assertEqual((report['sourceScreenCount'], report['outputScreenCount']), (17, 18))
            self.assertEqual(report['addedFiles'], ['screens/LocalControl.json'])
            self.assertEqual(set(report['changedFiles']), module.MODIFIED)
            self.assertEqual(module.hashes(SOURCE), before)
            self.assertEqual((project / 'manifest.json').read_bytes(),
                             (SOURCE / 'manifest.json').read_bytes().replace(
                                 b'"2.1.3-compact"', b'"2.1.4-local-control"'))
            expected_manifest = module.read(SOURCE / 'manifest.json')
            expected_manifest['packageVersion'] = '2.1.4-local-control'
            self.assertEqual(module.read(project / 'manifest.json'), expected_manifest)
            self.assertEqual(report['metadataDifferences'], {
                'packageVersion': {'before': '2.1.3-compact', 'after': '2.1.4-local-control'}})
            for name in before:
                if name not in module.MODIFIED:
                    self.assertEqual((SOURCE / name).read_bytes(), (project / name).read_bytes(), name)
            for name in ('tags.json', 'runtime-map.json'):
                original = module.read(SOURCE / name)
                updated = module.read(project / name)
                self.assertEqual(updated[:len(original)], original)
                self.assertEqual(len(updated), len(original) + 10)
            original = module.read(SOURCE / 'permissions.json')
            updated = module.read(project / 'permissions.json')
            updated['localAccess']['protectedScreenPrefixes'].remove('LocalControl')
            self.assertEqual(updated, original)
            original = module.read(SOURCE / 'screens/Control-Pcs.json')
            updated = module.read(project / 'screens/Control-Pcs.json')
            old = module.unique(original['widgets'], 'widgetId')
            new = module.unique(updated['widgets'], 'widgetId')
            for key in old:
                if key not in ('control-pcs-stop', 'control-pcs-phase-power'):
                    self.assertEqual(new[key], old[key])
            stop = copy.deepcopy(new['control-pcs-stop'])
            for key in ('controlPauseGuard', 'pauseGuardModeTag', 'pauseGuardAppliedTag'):
                self.assertIn(key, stop['properties'])
                del stop['properties'][key]
            self.assertEqual(stop, old['control-pcs-stop'])
            self.assertNotIn('control-pcs-phase-power', new)
            local = module.read(project / 'screens/LocalControl.json')
            self.assertEqual(len([w for w in local['widgets'] if w['type'] == 'qtInput']), 3)
            live_chrome = [w for w in original['widgets'] if w['widgetId'].startswith(('chrome-', 'nav-'))]
            new_chrome = [w for w in local['widgets'] if w['widgetId'].startswith(('chrome-', 'nav-'))]
            self.assertEqual(new_chrome, live_chrome)
            self.assertEqual(module.read(project / 'manifest.json')['entryScreen'], 'Overview')
            self.assertEqual(module.read(project / 'manifest.json')['packageVersion'], '2.1.4-local-control')
            self.assertFalse(report['modeInitialValueTouched'])
            actual = module.hashes(project)
            for name, expected in module.read(project / 'checksums.json').items():
                self.assertEqual(actual[name], expected)

    def test_never_overwrites_output(self):
        with tempfile.TemporaryDirectory() as temp:
            with self.assertRaisesRegex(ValueError, 'new directory'):
                module.overlay(SOURCE, CONTROLS, Path(temp))

    def test_template_collision_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            source = Path(temp) / 'source'
            module.shutil.copytree(SOURCE, source)
            routes = module.read(source / 'runtime-map.json')
            routes[0]['index'] = 740100
            module.write(source / 'runtime-map.json', routes)
            module.write(source / 'checksums.json', {k: v for k, v in module.hashes(source).items() if k != 'checksums.json'})
            with self.assertRaisesRegex(ValueError, 'already exists'):
                module.overlay(source, CONTROLS, Path(temp) / 'output')

    def test_modified_snapshot_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            source = Path(temp) / 'source'
            module.shutil.copytree(SOURCE, source)
            with (source / 'screens/Overview.json').open('a') as f:
                f.write(' ')
            with self.assertRaisesRegex(ValueError, 'checksum mismatch'):
                module.overlay(source, CONTROLS, Path(temp) / 'output')


if __name__ == '__main__':
    unittest.main()
