#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import os
import pathlib
import stat
import tempfile
import unittest
from types import SimpleNamespace


ROOT = pathlib.Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "deploy" / "verify-scada-active-project.py"
SPEC = importlib.util.spec_from_file_location("verify_scada_active_project", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
VERIFIER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(VERIFIER)


class ScadaTreeManifestTests(unittest.TestCase):
    def test_directory_st_size_is_fixed_across_filesystems(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary) / "project"
            (root / "screens").mkdir(parents=True)
            (root / "manifest.json").write_text("{}\n", encoding="utf-8")
            (root / "screens" / "overview.json").write_text("{}\n", encoding="utf-8")

            real_lstat = os.lstat

            def snapshot_with_directory_size(directory_size: int):
                def fake_lstat(path: os.PathLike[str] | str):
                    actual = real_lstat(path)
                    if stat.S_ISDIR(actual.st_mode):
                        return SimpleNamespace(
                            st_mode=actual.st_mode,
                            st_uid=actual.st_uid,
                            st_gid=actual.st_gid,
                            st_size=directory_size,
                        )
                    return actual

                VERIFIER.os.lstat = fake_lstat
                try:
                    return VERIFIER.inspect_tree(root)
                finally:
                    VERIFIER.os.lstat = real_lstat

            small = snapshot_with_directory_size(64)
            large = snapshot_with_directory_size(65536)
            self.assertEqual(
                small["canonicalManifestSha256"],
                large["canonicalManifestSha256"],
            )
            self.assertEqual(
                small["contentManifestSha256"],
                large["contentManifestSha256"],
            )
            self.assertEqual("fixed-4096", VERIFIER.DIRECTORY_SIZE_POLICY)


if __name__ == "__main__":
    unittest.main()
