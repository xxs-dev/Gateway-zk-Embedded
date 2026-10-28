import copy
import unittest

from comm108_fast26_profile import FAST, SAFETY, build_profile


def fixture():
    indexes = sorted(FAST | SAFETY)
    indexes += list(range(20000, 20000 + 440 - len(indexes)))
    return dict(protocol={'transport': {'frameIntervalMs': 50}},
        collect=dict(defaultIntervalMs=1000, maxBatchRegisters=32, maxRequestRegisters=32),
        meters=[dict(points=[dict(index=i, address=i-9820 if i in FAST else i,
            enabled=True, collectPriority=0 if i < 20020 else 1,
            read=dict(enable=True, function=4, length=1, dataType='uint16',
                      intervalMs=1000 if i < 20020 else 60000),
            write=dict(enable=False)) for i in indexes])])


class Tests(unittest.TestCase):
    def test_profile_changes_only_scheduling(self):
        original = fixture(); saved = copy.deepcopy(original)
        result = build_profile(original)
        self.assertEqual(original, saved)
        self.assertEqual(result['protocol']['transport']['frameIntervalMs'], 20)
        self.assertNotIn('maxBackgroundTasksPerMeterPerCycle', result['collect'])
        for old, new in zip(original['meters'][0]['points'], result['meters'][0]['points']):
            if new['index'] in FAST:
                self.assertEqual((new['collectPriority'], new['read']['intervalMs']), (0, 20))
            elif new['index'] in SAFETY:
                self.assertEqual((new['collectPriority'], new['read']['intervalMs']), (1, 1000))
            else:
                self.assertEqual(new['read']['intervalMs'], max(5000, old['read']['intervalMs']))
            new['collectPriority'] = old['collectPriority']
            new['read']['intervalMs'] = old['read']['intervalMs']
            self.assertEqual(new, old)

    def test_unsafe_or_different_table_rejected(self):
        for kind in ('write', 'startup', 'missing', 'address', 'cap'):
            c = fixture()
            if kind == 'write': c['meters'][0]['points'][0]['write']['enable'] = True
            elif kind == 'startup': c['startupWrites'] = [{}]
            elif kind == 'missing': c['meters'][0]['points'].pop()
            elif kind == 'cap': c['collect']['maxBatchRegisters'] = 20
            else:
                next(p for p in c['meters'][0]['points'] if p['index'] in FAST)['address'] += 1
            with self.subTest(kind=kind), self.assertRaises(ValueError): build_profile(c)


if __name__ == '__main__': unittest.main()
