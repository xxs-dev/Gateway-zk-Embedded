import copy
import unittest
from scada_capture_pipeline import normalize, validate, meter_batches, safety_disabled


class PipelineTest(unittest.TestCase):
    def setUp(self):
        self.point = dict(index=740100, value=1, quality=0, ts=10, expireAt=15, stale=True)
        self.contract = dict(pointsPointer='/points', fields={k: '/' + k for k in self.point})

    def test_preserves_bad_quality_and_stale(self):
        result = normalize({'points': [self.point]}, self.contract, 20)
        self.assertEqual(result['points'], [self.point])

    def test_missing_metadata_is_not_defaulted(self):
        for key in self.point:
            if key == 'expireAt':
                continue
            point = copy.deepcopy(self.point)
            del point[key]
            with self.assertRaises(KeyError):
                normalize({'points': [point]}, self.contract, 20)

    def test_duplicate_and_nonfinite_rejected(self):
        with self.assertRaises(ValueError):
            normalize({'points': [self.point, self.point]}, self.contract, 20)
        self.point['value'] = float('nan')
        with self.assertRaises(ValueError):
            normalize({'points': [self.point]}, self.contract, 20)

    def test_both_explicit_approvals_required(self):
        for user, kepler in [(False, False), (True, False), (False, True), ('yes', True)]:
            with self.assertRaisesRegex(ValueError, 'STOP'):
                validate(dict(userConfirmedDeployed=user, keplerAccepted=kepler))

    def test_batches_cover_more_than_2000_routes(self):
        routes = [{'index': i, 'meterCode': 'm' + str(i // 1000)} for i in range(2105)]
        batches = meter_batches(routes)
        self.assertEqual(len(batches), 3)
        self.assertEqual([i for _, batch in batches for i in batch], list(range(2105)))

    def test_safety_flag_must_be_explicit_false(self):
        safety_disabled({'enabled': False}, '/enabled')
        for value in (True, None, 0, 'false'):
            with self.assertRaises(ValueError):
                safety_disabled({'enabled': value}, '/enabled')

    def test_actual_routes_join_tags_for_meter(self):
        self.assertEqual(meter_batches([{'index': 1, 'tagId': 'a'}],
                                      [{'tagId': 'a', 'meterCode': 'EMS_CORE'}]), [('EMS_CORE', [1])])

    def test_missing_expiry_is_unknown_with_provenance(self):
        del self.point['expireAt']
        result = normalize({'points': [self.point]}, self.contract, 20)
        self.assertEqual(result['missingExpireAtIndexes'], [740100])
        self.assertEqual(result['points'][0], dict(self.point, expireAt=0))
        self.point['expireAt'] = None
        with self.assertRaises(ValueError):
            normalize({'points': [self.point]}, self.contract, 20)

    def test_absent_safety_requires_matching_version_evidence(self):
        proof = dict(effectivePointer='/enabled', defaultValue=False, binaryPath='/bin/test',
                     binarySha256='a'*64, versionEvidence='reviewed build manifest',
                     modelDefaultEvidence='model default false', effectiveLoaderEvidence='effective assignment reviewed')
        result = safety_disabled({}, '/enabled', proof, 'a'*64)
        self.assertEqual(result['basis'], 'version-verified-default-false')
        for evidence, digest in [(None, None), (proof, 'b'*64), (dict(proof, effectiveLoaderEvidence=''), 'a'*64)]:
            with self.assertRaises(ValueError):
                safety_disabled({}, '/enabled', evidence, digest)
        with self.assertRaises(ValueError):
            safety_disabled({'enabled': True}, '/enabled', proof, 'a'*64)


if __name__ == '__main__':
    unittest.main()
