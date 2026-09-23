#!/usr/bin/env python3
"""Reject unpinned or dirty inputs before generic package assembly."""
import json
from pathlib import Path
import tempfile
import unittest

import build_generic_factory as builder


class ProgramProvenanceTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.programs = self.root / "programs.tar.gz"
        self.programs.write_bytes(b"fixture, not executable")
        self.manifest = self.root / "manifest.json"
        self.commit = "1" * 40
        self.data = {"sourceCommit": self.commit, "sourceDirty": False, "components": []}
        self.write_manifest()

    def write_manifest(self):
        self.manifest.write_text(json.dumps(self.data), encoding="utf-8")

    def verify(self, commit=None, program_sha=None, manifest_sha=None):
        return builder.verify_program_inputs(
            self.programs, self.manifest, commit or self.commit,
            program_sha or builder.sha(self.programs), manifest_sha or builder.sha(self.manifest))

    def test_explicit_pinned_source(self):
        self.assertEqual(self.data, self.verify())

    def test_archive_digest_mismatch(self):
        with self.assertRaisesRegex(ValueError, "SHA mismatch"):
            self.verify(program_sha="0" * 64)

    def test_manifest_digest_mismatch(self):
        with self.assertRaisesRegex(ValueError, "SHA mismatch"):
            self.verify(manifest_sha="0" * 64)

    def test_wrong_commit(self):
        with self.assertRaisesRegex(ValueError, "provenance"):
            self.verify(commit="2" * 40)

    def test_dirty_and_ambiguous_dirty_fields(self):
        for value in (True, None, 0, "false"):
            with self.subTest(value=value):
                self.data["sourceDirty"] = value
                self.write_manifest()
                with self.assertRaisesRegex(ValueError, "provenance"):
                    self.verify()

    def test_abbreviated_or_invalid_pins(self):
        for kwargs in ({"commit": "1" * 7}, {"commit": "X" * 40},
                       {"program_sha": "a" * 63}, {"manifest_sha": "g" * 64}):
            with self.subTest(kwargs=kwargs):
                with self.assertRaisesRegex(ValueError, "hexadecimal"):
                    self.verify(**kwargs)


if __name__ == "__main__":
    unittest.main()
