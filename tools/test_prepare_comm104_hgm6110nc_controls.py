import unittest
from prepare_comm104_hgm6110nc_controls import COMMANDS, METER, bind_controls, control_points


class ControlsTest(unittest.TestCase):
    def baseline(self):
        config = dict(protocol=dict(slave=1), meters=[dict(meterCode=METER, model='HGM6110NC', points=[
            dict(index=160111, pointCode='gen_fc03_0034', valueMap={'0':'待机', '9':'正常运行'})])])
        tags = [dict(tagId=METER+'.'+feedback) for _, _, _, feedback, _, _ in COMMANDS if feedback]
        page = dict(widgets=[dict(widgetId=name, title='', properties={}, geometry={}) for name in
                            ('control-mode-heading', 'diesel-empty', 'control-mode-title', 'control-mode-value', 'control-switch-mode')])
        return config, tags, [], page

    def test_addresses_and_only_true(self):
        points = control_points()
        self.assertEqual([p['address'] for p in points], list(range(7)))
        self.assertEqual(points[5]['pointCode'], 'gen_cmd_open')
        self.assertEqual(points[6]['pointCode'], 'gen_cmd_close')
        for p in points:
            self.assertFalse(p['read']['enable'])
            self.assertEqual(p['write']['function'], 5)
            self.assertEqual(p['write']['allowedValues'], [1])
            self.assertEqual((p['write']['min'], p['write']['max']), (1, 1))
            self.assertFalse(p['write']['verifyByRead'])
            self.assertNotIn('startupValue', p['write'])

    def test_confirmed_one_shot_and_separate_feedback(self):
        inputs = self.baseline()
        config, tags, maps, page = bind_controls(*inputs)
        self.assertEqual(len(inputs[0]['meters'][0]['points']), 1)
        self.assertEqual(len(config['meters'][0]['points']), 8)
        self.assertTrue(all(m['writable'] for m in maps))
        buttons = [w for w in page['widgets'] if w['widgetId'].startswith('gen-control-')]
        self.assertEqual(len(buttons), 7)
        for w in buttons:
            self.assertEqual(w['action']['type'], 'writeSetpoint')
            self.assertEqual(w['action']['value'], '1')
            self.assertTrue(w['action']['requiresConfirmation'])
            self.assertTrue(w['properties']['controlWarning'])
            if w['widgetId'] in ('gen-control-open', 'gen-control-close'):
                self.assertNotIn('controlFeedbackTag', w['properties'])
            else:
                self.assertNotEqual(w['action']['tagId'], w['properties']['controlFeedbackTag'])

    def test_duplicate_rejected(self):
        inputs = bind_controls(*self.baseline())
        with self.assertRaises(AssertionError):
            bind_controls(*inputs)

    def test_wrong_family_rejected(self):
        c, t, m, p = self.baseline()
        c['meters'][0]['model'] = 'HGM6120'
        with self.assertRaises(AssertionError):
            bind_controls(c, t, m, p)


if __name__ == '__main__':
    unittest.main()
