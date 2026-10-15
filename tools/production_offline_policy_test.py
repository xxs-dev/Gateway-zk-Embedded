"""Offline runtime policy is explicit, fail-closed and never OTA authority."""
import copy
import contextlib
import importlib.util
import os
from pathlib import Path
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('offline_guard', ROOT / 'deploy/runtime-upgrade-guard.py')
guard = importlib.util.module_from_spec(spec)
spec.loader.exec_module(guard)


class OfflinePolicyTest(unittest.TestCase):
    def setUp(self):
        self.offline = dict(otaEnabled=False, requireSignature=True, checksumRequired=True,
                            allowInsecureHttp=False, publicKeySha256=None, httpsOrigins=[])
        self.signed = dict(requireSignature=True, checksumRequired=True, allowInsecureHttp=False,
                           publicKeySha256='a' * 64, httpsOrigins=['https://release.example'])

    def test_offline_never_reads_or_installs_a_key(self):
        with mock.patch.object(guard, 'production_pin', side_effect=AssertionError('key access')):
            self.assertFalse(guard.production_trust_verify(self.offline, allow_disabled=True))

    def test_legacy_policy_still_requires_exact_installed_key(self):
        with mock.patch.object(guard, 'production_pin', return_value={'sha256': 'a' * 64}):
            self.assertTrue(guard.production_trust_verify(self.signed))
        for error in (ValueError('invalid key'), FileNotFoundError('no key')):
            with mock.patch.object(guard, 'production_pin', side_effect=error):
                with self.assertRaises((ValueError, FileNotFoundError)):
                    guard.production_trust_verify(self.signed)
        with mock.patch.object(guard, 'production_pin', return_value={'sha256': 'b' * 64}):
            with self.assertRaisesRegex(ValueError, 'fingerprint drift'):
                guard.production_trust_verify(self.signed)

    def test_offline_requires_v2_opt_in_and_exact_security_policy(self):
        with self.assertRaises(ValueError):
            guard.production_trust_policy(self.offline)
        for key, value in (('otaEnabled', 0), ('otaEnabled', None), ('otaEnabled', True),
                ('requireSignature', False), ('checksumRequired', False), ('allowInsecureHttp', True),
                ('publicKeySha256', 'a' * 64), ('httpsOrigins', ['https://release.example']),
                ('unknown', True)):
            with self.subTest(key=key, value=value):
                with self.assertRaises(ValueError):
                    guard.production_trust_policy(dict(self.offline, **{key: value}), allow_disabled=True)
        for key in self.offline:
            value = copy.deepcopy(self.offline)
            del value[key]
            with self.subTest(missing=key), self.assertRaises(ValueError):
                guard.production_trust_policy(value, allow_disabled=True)

    def test_signed_missing_or_insecure_origins_never_fall_back_offline(self):
        for origins in ([], ['http://release.example'], ['https://release.example'] * 2, [None], None):
            with self.subTest(origins=origins), self.assertRaises(ValueError):
                guard.production_trust_policy(dict(self.signed, httpsOrigins=origins), allow_disabled=True)

    def test_ota_admission_is_separate_from_local_runtime(self):
        with self.assertRaisesRegex(ValueError, 'OTA is disabled'):
            guard.production_require_ota(dict(format='runtime-baseline-v2', trust=self.offline))
        guard.production_require_ota(dict(format='runtime-baseline-v2', trust=self.signed))

    @unittest.skipUnless(os.name == 'posix', 'Linux lifecycle module')
    def test_ota_owner_refuses_before_opening_or_staging_artifact(self):
        spec = importlib.util.spec_from_file_location('offline_owner', ROOT / 'deploy/offline-runtime-upgrade.py')
        lifecycle = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(lifecycle)
        protocol = mock.Mock(root=Path('/opt/modbus-gateway/ota/staging'))
        protocol.intent.return_value = {}
        baseline = dict(format='runtime-baseline-v2', trust=self.offline)
        with mock.patch.object(lifecycle, 'production_lock', return_value=contextlib.nullcontext()), \
                mock.patch.object(lifecycle.guard, 'production_context', return_value=baseline), \
                mock.patch.object(lifecycle, 'regular', side_effect=AssertionError('artifact access')):
            with self.assertRaisesRegex(ValueError, 'OTA is disabled'):
                lifecycle.production_ota_owner(protocol, {})
        protocol.artifact_hash.assert_not_called()


if __name__ == '__main__':
    unittest.main()
