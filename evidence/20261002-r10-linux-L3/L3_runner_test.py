"""Local L3 manifest and admission tests; no SSH or compiler invocation."""
import ast
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import stat
import sys
import types
import unittest
from unittest import mock

HERE = Path(__file__).resolve().parent


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


inputs = load('l3_inputs_test', HERE / 'prepare-l3.py')
runner = load('l3_runner_test', HERE / 'execute-l3.py')


class L3RunnerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.archive, cls.manifest = inputs.prepare()
        cls.frozen = json.loads((HERE / 'input-manifest-L3.json').read_bytes())

    def test_frozen_git_archive_and_artifact_scope(self):
        self.assertEqual(self.manifest, self.frozen)
        self.assertEqual(self.manifest['sourceCommit'], inputs.COMMIT)
        self.assertEqual(self.manifest['artifactNames'], inputs.TARGETS + ['ownership-probe'])
        self.assertEqual(len(self.manifest['artifactNames']), 5)
        self.assertTrue(self.manifest['gitByteIdentity'])
        self.assertFalse(self.manifest['executionAuthorized'])
        self.assertFalse(self.manifest['boardExecutionReady'])
        self.assertTrue(all(not row['path'].startswith('evidence/') for row in self.manifest['sourceFiles']))

    def test_default_dry_run_does_not_load_authentication(self):
        output = io.StringIO()
        with mock.patch.object(runner.inputs, 'prepare', return_value=(self.archive, self.manifest)), \
             mock.patch.object(runner.inputs, 'primitives', return_value={}), \
             mock.patch.object(runner.importlib.util, 'spec_from_file_location', side_effect=AssertionError('authentication import')), \
             mock.patch.object(sys, 'argv', ['execute-l3.py']), contextlib.redirect_stdout(output):
            self.assertEqual(runner.main(), 0)
        plan = json.loads(output.getvalue())
        self.assertFalse(plan['connects'])
        self.assertFalse(plan['remoteResultAvailable'])
        self.assertFalse((HERE / 'L3-build-result.json').exists())

    def test_drift_and_wrong_pin_refuse_before_authentication(self):
        changed = dict(self.manifest, buildRoot='/srv/build/other')
        for argv, value, reason in ((['execute-l3.py'], changed, 'fixed L3 input drift'),
                                    (['execute-l3.py', '--manifest-sha256', '0' * 64], self.manifest, '')):
            with self.subTest(argv=argv), mock.patch.object(runner.inputs, 'prepare', return_value=(self.archive, value)), \
                 mock.patch.object(runner.importlib.util, 'spec_from_file_location', side_effect=AssertionError('authentication import')), \
                 mock.patch.object(sys, 'argv', argv), self.assertRaises(AssertionError) as failure:
                runner.main()
            self.assertEqual(str(failure.exception), reason)
        self.assertFalse((HERE / 'L3-build-result.json').exists())

    def test_execution_flag_requires_exact_manifest_before_authentication(self):
        with mock.patch.object(runner.inputs, 'prepare', return_value=(self.archive, self.manifest)), \
             mock.patch.object(runner.inputs, 'primitives', return_value={}), \
             mock.patch.object(runner.importlib.util, 'spec_from_file_location', side_effect=AssertionError('authentication import')), \
             mock.patch.object(sys, 'argv', ['execute-l3.py', '--execute-approved']), self.assertRaisesRegex(AssertionError, 'exact approved manifest required'):
            runner.main()
        self.assertFalse((HERE / 'L3-build-result.json').exists())

    def test_remote_command_is_bound_to_five_artifacts_and_hash(self):
        code = runner.remote_code(self.manifest, {'bootId': 'fixture'})
        ast.parse(code, feature_version=(3, 6))
        self.assertIn(self.manifest['buildRoot'], code)
        self.assertIn(self.manifest['supplementalFiles']['build-child.py']['sha256'], code)
        self.assertIn("'SystemMonitor'", code)
        self.assertIn("'ownership-probe'", code)

    def test_sftp_parent_rejects_wrong_type_owner_or_mode(self):
        expected = dict(uid=0, gid=0, mode=0o755)
        good = types.SimpleNamespace(st_mode=stat.S_IFDIR | 0o755, st_uid=0, st_gid=0)
        runner.check_sftp_parent(good, expected)
        for row in (types.SimpleNamespace(st_mode=stat.S_IFLNK | 0o755, st_uid=0, st_gid=0),
                    types.SimpleNamespace(st_mode=stat.S_IFDIR | 0o777, st_uid=0, st_gid=0),
                    types.SimpleNamespace(st_mode=stat.S_IFDIR | 0o755, st_uid=1000, st_gid=0)):
            with self.subTest(row=row), self.assertRaises(AssertionError):
                runner.check_sftp_parent(row, expected)


if __name__ == '__main__':
    unittest.main(verbosity=2)
