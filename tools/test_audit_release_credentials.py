#!/usr/bin/env python3

from __future__ import print_function

import io
import json
import os
import sys
import tarfile
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import audit_release_credentials as audit


class ReleaseCredentialAuditTest(unittest.TestCase):
    def setUp(self):
        self.temp_dir = tempfile.TemporaryDirectory(prefix="gateway-credential-audit-")

    def tearDown(self):
        self.temp_dir.cleanup()

    def write_json(self, relative_path, value):
        path = os.path.join(self.temp_dir.name, relative_path)
        parent = os.path.dirname(path)
        if not os.path.isdir(parent):
            os.makedirs(parent)
        with open(path, "w", encoding="utf-8", newline="\n") as stream:
            json.dump(value, stream)
            stream.write("\n")
        return path

    def test_empty_sensitive_fields_are_allowed(self):
        path = self.write_json(
            "clean.json",
            {
                "password": "",
                "accessKey": "",
                "secretKey": "",
                "token": "",
                "tokenParam": "token",
            },
        )
        result = audit.audit_paths([path])
        self.assertEqual([], result.errors)
        self.assertEqual([], result.findings)

    def test_non_empty_sensitive_json_is_rejected_without_echoing_value(self):
        sentinel = "synthetic-value-must-not-be-printed"
        path = self.write_json("unsafe.json", {"nested": [{"accessKey": sentinel}]})
        stdout = io.StringIO()
        stderr = io.StringIO()
        self.assertEqual(1, audit.run([path], stdout=stdout, stderr=stderr))
        self.assertIn("/nested/0/accessKey", stderr.getvalue())
        self.assertNotIn(sentinel, stdout.getvalue() + stderr.getvalue())

    def test_private_key_marker_is_rejected(self):
        path = os.path.join(self.temp_dir.name, "client.pem")
        with open(path, "wb") as stream:
            stream.write(b"-----BEGIN PRIVATE KEY-----\nsynthetic-test-only\n-----END PRIVATE KEY-----\n")
        result = audit.audit_paths([path])
        self.assertEqual(1, len(result.findings))
        self.assertEqual("private key marker", result.findings[0].reason)

    def test_tar_archive_is_scanned_without_extraction(self):
        payload = self.write_json("payload/config.json", {"auth": {"token": "synthetic-token"}})
        archive_path = os.path.join(self.temp_dir.name, "candidate.tar.gz")
        with tarfile.open(archive_path, "w:gz") as archive:
            archive.add(payload, arcname="gateway/config.json")
        result = audit.audit_paths([archive_path])
        self.assertEqual([], result.errors)
        self.assertEqual(1, len(result.findings))
        self.assertIn("gateway/config.json", result.findings[0].source)

    def test_tar_archive_inside_directory_is_scanned(self):
        payload = self.write_json("payload/config.json", {"auth": {"password": "synthetic"}})
        archive_path = os.path.join(self.temp_dir.name, "candidate.tar.gz")
        with tarfile.open(archive_path, "w:gz") as archive:
            archive.add(payload, arcname="gateway/config.json")
        os.remove(payload)
        result = audit.audit_paths([self.temp_dir.name])
        self.assertEqual([], result.errors)
        self.assertEqual(1, len(result.findings))

    def test_nested_tar_archive_is_scanned(self):
        payload = self.write_json("payload/config.json", {"auth": {"token": "synthetic"}})
        inner = io.BytesIO()
        with tarfile.open(fileobj=inner, mode="w:gz") as archive:
            archive.add(payload, arcname="gateway/config.json")
        outer_path = os.path.join(self.temp_dir.name, "candidate.tar.gz")
        with tarfile.open(outer_path, "w:gz") as archive:
            info = tarfile.TarInfo("bundles/inner.tar.gz")
            info.size = len(inner.getvalue())
            archive.addfile(info, io.BytesIO(inner.getvalue()))
        result = audit.audit_paths([outer_path])
        self.assertEqual([], result.errors)
        self.assertEqual(1, len(result.findings))

    def test_embedded_url_credentials_are_rejected(self):
        path = self.write_json(
            "url.json",
            {
                "userinfo": "https://user:synthetic@example.invalid/file",
                "query": "https://example.invalid/file?apiToken=synthetic",
            },
        )
        result = audit.audit_paths([path])
        self.assertEqual([], result.errors)
        self.assertEqual(2, len(result.findings))

    def test_repository_release_inputs_are_clean(self):
        root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        inputs = [os.path.join(root, "config"), os.path.join(root, "gateway-factory-defaults.tar.gz")]
        result = audit.audit_paths(inputs)
        details = "\n".join(
            "{}:{}: {}".format(item.source, item.location, item.reason)
            for item in result.findings
        )
        self.assertEqual([], result.errors)
        self.assertEqual([], result.findings, details)


if __name__ == "__main__":
    unittest.main()
