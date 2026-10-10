"""Nonroot, inert local tests of the production AGC preflight source contract."""
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest

spec = importlib.util.spec_from_file_location('runtime', Path(__file__).resolve().parents[1] / 'deploy/prepare-generic-runtime.py')
runtime = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runtime)


def pin(data):
    return {'sha256': hashlib.sha256(data).hexdigest(), 'sizeBytes': len(data)}


class MultiOriginPreflightTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.package = self.root / 'package'
        self.deploy = self.root / 'upload'
        self.package.mkdir()
        self.deploy.mkdir()
        self.origins, self.generic_files, self.overlay_files = {}, {}, {}
        for index in range(20):
            name = 'bin/AgcAvcController' if index == 0 else 'ky-ems/KY-EMS' if index == 19 else 'bin/Program' + str(index)
            path = name if name.startswith('ky-ems/') else 'build-aarch64/' + name[4:]
            data = ('inert component ' + str(index)).encode()
            self.put(path, data)
            (self.overlay_files if index == 0 else self.generic_files)[path] = pin(data)['sha256']
            self.origins[name] = {'binary': pin(data), 'sourceCommitBase': ('c' if index == 5 else 'b') * 40,
                                  'sourceDirty': index != 5, 'sourceArchive': pin(b'inert source archive'),
                                  'sourceInputs': pin(b'inert source inventory'), 'originalPacket': pin(b'inert original packet'),
                                  'buildSeal': pin(b'inert original seal')}
        data = b'inert helper never executed'
        self.put('deploy/helper.sh', data)
        (self.deploy / 'helper.sh').write_bytes(data)
        self.generic_files['deploy/helper.sh'] = pin(data)['sha256']
        self.put('edge-package-manifest.json', b'{"components":[]}')
        self.generic_files['edge-package-manifest.json'] = pin(b'{"components":[]}')['sha256']
        common = {'binarySourceCommit': None, 'binarySourceCommits': ['b' * 40, 'c' * 40],
                  'assemblySourceCommit': 'a' * 40, 'assemblySourceDirty': True,
                  'sourceDirtyScope': 'ORIGINAL_BUILD_INPUTS_NOT_CURRENT_CHECKOUT', 'componentOrigins': self.origins}
        self.generic = {**copy.deepcopy(common), 'files': self.generic_files}
        self.overlay = {**copy.deepcopy(common), 'sourceCommit': 'b' * 40,
                        'sourceCommitScope': 'AGC_COMPONENT_ORIGINAL_BUILD_BASE', 'files': self.overlay_files}
        self.args = SimpleNamespace(package_root=self.package, deploy_dir=self.deploy, mode='agc_avc',
                                    machine='LOCAL_INERT_001', profile='full', manifest=None, qt='0',
                                    start='0', selection=self.root / 'private/selection.json', home=self.root / 'home')

    def put(self, name, data):
        path = self.package / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)

    def manifests(self):
        self.put('generic-package.json', json.dumps(self.generic).encode())
        self.put('agc-avc-runtime-manifest.json', json.dumps(self.overlay).encode())

    def test_actual_preflight_accepts_complete_multi_origin(self):
        self.manifests()
        runtime.preflight(self.args)
        self.assertEqual({'requiredDrivers': []}, runtime.read(self.args.selection))
        self.assertFalse(self.args.home.exists())

    def test_legacy_single_origin_remains_strict(self):
        self.generic = {'binarySourceCommit': 'b' * 40, 'files': self.generic_files}
        self.overlay = {'sourceCommit': 'b' * 40, 'files': self.overlay_files}
        self.manifests()
        runtime.preflight(self.args)
        for source in (None, 'c' * 40, 'b' * 40 + '\n'):
            with self.subTest(source=source):
                self.overlay['sourceCommit'] = source
                self.manifests()
                with self.assertRaisesRegex(ValueError, 'AGC overlay source'):
                    runtime.preflight(self.args)

    def test_multi_origin_rejects_missing_wrong_pin_sets_and_types(self):
        original_generic, original_overlay = copy.deepcopy(self.generic), copy.deepcopy(self.overlay)
        for defect in ('missing', 'extra', 'null-null', 'wrong-binary', 'wrong-size', 'wrong-archive', 'bad-archive',
                       'wrong-source', 'wrong-commit-set', 'duplicate-commit', 'dirty-type', 'assembly-type',
                       'scope', 'agc-scope', 'wrong-assembly', 'forged-single', 'missing-evidence', 'missing-file-binding',
                       'coherent-wrong-size', 'coherent-wrong-pin', 'bad-file-bytes'):
            with self.subTest(defect=defect):
                self.generic, self.overlay = copy.deepcopy(original_generic), copy.deepcopy(original_overlay)
                origins = self.overlay['componentOrigins']
                agc = origins['bin/AgcAvcController']
                if defect == 'missing':
                    del origins['bin/AgcAvcController']
                elif defect == 'extra':
                    origins['bin/Extra'] = copy.deepcopy(agc)
                elif defect == 'null-null':
                    self.generic.pop('componentOrigins')
                    self.overlay.pop('componentOrigins')
                    self.overlay['sourceCommit'] = None
                elif defect == 'wrong-binary':
                    agc['binary']['sha256'] = '0' * 64
                elif defect == 'wrong-size':
                    agc['binary']['sizeBytes'] = True
                elif defect == 'wrong-archive':
                    agc['sourceArchive']['sha256'] = '0' * 64
                elif defect == 'bad-archive':
                    agc['sourceArchive']['sha256'] += '\n'
                    self.generic['componentOrigins'] = copy.deepcopy(origins)
                elif defect == 'wrong-source':
                    self.overlay['sourceCommit'] = 'c' * 40
                elif defect == 'wrong-commit-set':
                    self.overlay['binarySourceCommits'] = ['b' * 40]
                elif defect == 'duplicate-commit':
                    self.overlay['binarySourceCommits'].append('b' * 40)
                elif defect == 'dirty-type':
                    agc['sourceDirty'] = 1
                    self.generic['componentOrigins'] = copy.deepcopy(origins)
                elif defect == 'assembly-type':
                    self.generic['assemblySourceDirty'] = self.overlay['assemblySourceDirty'] = 1
                elif defect == 'scope':
                    self.overlay['sourceDirtyScope'] = 'CURRENT_CHECKOUT'
                elif defect == 'agc-scope':
                    self.overlay.pop('sourceCommitScope')
                elif defect == 'wrong-assembly':
                    self.overlay['assemblySourceCommit'] = 'c' * 40
                elif defect == 'forged-single':
                    self.generic['binarySourceCommit'] = self.overlay['binarySourceCommit'] = 'b' * 40
                elif defect == 'missing-evidence':
                    del agc['buildSeal']
                    self.generic['componentOrigins'] = copy.deepcopy(origins)
                elif defect == 'missing-file-binding':
                    self.overlay['files'] = {}
                elif defect == 'coherent-wrong-size':
                    agc['binary']['sizeBytes'] += 1
                    self.generic['componentOrigins'] = copy.deepcopy(origins)
                elif defect == 'coherent-wrong-pin':
                    agc['binary']['sha256'] = '0' * 64
                    self.overlay['files']['build-aarch64/AgcAvcController'] = '0' * 64
                    self.generic['componentOrigins'] = copy.deepcopy(origins)
                else:
                    self.put('build-aarch64/AgcAvcController', b'inert tampered bytes')
                self.manifests()
                with self.assertRaises(ValueError):
                    runtime.preflight(self.args)
                self.assertFalse(self.args.home.exists())

    def test_existing_paired_deploy_validation_not_bypassed(self):
        self.manifests()
        (self.deploy / 'helper.sh').write_bytes(b'inert tampered upload')
        with self.assertRaisesRegex(ValueError, 'paired deploy script mismatch'):
            runtime.preflight(self.args)

    def test_duplicate_origin_json_rejected_by_production_read(self):
        self.manifests()
        path = self.package / 'agc-avc-runtime-manifest.json'
        data = path.read_text()
        data = data.replace('"bin/AgcAvcController": {', '"bin/AgcAvcController": {}, "bin/AgcAvcController": {', 1)
        path.write_text(data)
        with self.assertRaisesRegex(ValueError, 'duplicate bundle manifest field'):
            runtime.preflight(self.args)


if __name__ == '__main__':
    unittest.main(verbosity=2)
