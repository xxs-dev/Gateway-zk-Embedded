#!/usr/bin/env python3
"""New credential-type and directory-symlink negatives only; no owner/devices."""
import importlib.util
import json
import os
from pathlib import Path
import tempfile
import unittest

SOURCE = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('credential_stage', SOURCE / 'deploy/package-credential-stage.py')
POLICY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(POLICY)


class CredentialPolicyTest(unittest.TestCase):
    def setUp(self):
        self.assertNotEqual(0, os.geteuid(), 'nonroot test only')
        self.temp = tempfile.TemporaryDirectory(prefix='package-policy-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.config = self.root / 'config'
        self.config.mkdir()

    def test_nonstring_credential_types_reject_without_retaining_values(self):
        path = self.config / 'input.json'
        for value in (None, False, 0, 1.25, [], {}, ['synthetic-only'], {'nested': 'synthetic-only'}):
            with self.subTest(type=type(value).__name__):
                raw = json.dumps({'ota': {'requireSignature': True}, 'credentials': {'password': value}}).encode('ascii')
                path.write_bytes(raw)
                with self.assertRaisesRegex(ValueError, 'credential field requires string'):
                    POLICY.stage(self.config)
                self.assertEqual(raw, path.read_bytes(), 'rejected input was mutated')

    def test_staging_directory_symlink_explicitly_rejected(self):
        outside = self.root / 'other-config'
        outside.mkdir()
        target = outside / 'input.json'
        raw = b'{"password":"synthetic-only"}\n'
        target.write_bytes(raw)
        (self.config / 'linked-directory').symlink_to(outside, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, 'symlinked staging directory'):
            POLICY.stage(self.config)
        self.assertEqual(raw, target.read_bytes(), 'external target was mutated')
        self.assertTrue((self.config / 'linked-directory').is_symlink())


if __name__ == '__main__':
    unittest.main()
