"""Offline migration tests. Set COMM104_GRAPH and COMM104_RUNTIME for local fixtures.

The node's hysteresis is tested by the C++ core suite. These tests exercise graph
selection, writer preservation, topology, point evidence and preparation failures.
"""
import copy
import importlib.util
import json
import os
import shutil
from pathlib import Path
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1]/'tools/prepare-grid-reserve.py'
SPEC = importlib.util.spec_from_file_location('prepare_grid_reserve', SCRIPT)
m = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(m)
HISTORY = Path('D:/workspace/Embedded/Gateway-zk-cleanup-backup-20260806-225840/generated/artifacts')
GRAPH = Path(os.environ.get('COMM104_GRAPH', str(HISTORY/'voltage-pv-refactor-20260806161736/shuntong_ems_modular_graph.COMM202600104.json')))
RUNTIME = Path(os.environ.get('COMM104_RUNTIME', str(HISTORY/'audits/COMM202600104-global-fault-20260730/snapshot/runtime')))


@unittest.skipUnless(GRAPH.is_file() and RUNTIME.is_dir(), 'Historical offline fixture unavailable')
class MigrationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = m.read(GRAPH)
        cls.graph = m.migrate_graph(cls.source, 41.6)
        cls.nodes = {n['id']: n for n in cls.graph['nodes']}
        cls.original = {n['id']: n for n in cls.source['nodes']}

    def evaluate(self, index, values):
        """Evaluate only migrated switch/gate wiring; controller is an injected boundary."""
        producers = {n['parameters']['outputIndex']: n for n in self.graph['nodes']
                     if n['id'].startswith('grid_reserve_') and 'outputIndex' in n['parameters']}
        def get(i):
            if i in values:
                return values[i]
            if i not in producers:
                raise AssertionError(f'Test boundary not supplied: {i}')
            node = producers[i]
            p = node['parameters']
            if node['type'] == 'controlGate':
                passed = []
                for c in p['conditions']:
                    left, right = get(c['index']), c['value']
                    passed.append({'eq': lambda: left == right, 'gt': lambda: left > right,
                        'lt': lambda: left < right, 'gte': lambda: left >= right,
                        'lte': lambda: left <= right}[c['operator']]())
                value = int(all(passed) if p['combine'] == 'all' else any(passed))
            else:
                branch = 'true' if get(p['conditionIndex']) else 'false'
                value = p.get(branch+'Value') if branch+'Value' in p else get(p[branch+'Index'])
            values[i] = value
            return value
        return get(index)

    def boundary(self, mode=3, soc=99.5, standby=1, charge_command=1):
        controller = self.nodes['grid_reserve_controller']['parameters']
        values = {m.MODE: mode, 1569: soc, 1570: 72, 161: 95,
                  1399: 1, 3999: 1, 1212: 0, 1215: 0, 1214: 1,
                  1216: 1, 1202: 0, 1462: 0, 8199: 0, 1279: 48059, 1556: 100, 1557: 100,
                  controller['standbyOutput']: standby,
                  controller['paOutput']: 1, controller['pbOutput']: 1, controller['pcOutput']: 1}
        values.update(zip(m.MANUAL, [2, -3, 4]))
        for name, value in [('cycle_charge_command_allowed', charge_command),
                            ('cycle_discharge_command_allowed', 1), ('cycle_direction_allowed', 1)]:
            values[self.original[name]['parameters']['outputIndex']] = value
        values[self.nodes['grid_reserve_stop_latch']['parameters']['stateOutputIndex']] = 0
        for i in range(3):
            values[self.original['power_constraints']['parameters']['activeInputIndexes'][i]] = 8+i
            values[self.original['power_constraints']['parameters']['reactiveInputIndexes'][i]] = 2+i
        return values

    def test_evidence_hash(self):
        self.assertIn(m.digest(GRAPH), (m.BASELINE_SHA256, m.CURRENT_SHA256))

    def test_auto_branches_preserved(self):
        for name in ['power_arbiter', 'cycle_phase_sequence', 'force_full_sequence',
                     'cycle_charge_direction_allowed', 'cycle_discharge_direction_allowed']:
            self.assertEqual(self.nodes[name], self.original[name])
        self.assertEqual(self.source, m.read(GRAPH))
        p = self.nodes['power_constraints']['parameters']
        for i, index in enumerate(p['activeInputIndexes']):
            self.assertEqual(self.evaluate(index, self.boundary(mode=1)), 8+i)
        values = self.boundary(mode=1)
        expected = values[self.original['power_constraints']['parameters']['stateIndex']]
        self.assertEqual(self.evaluate(p['stateIndex'], values), expected)

    def test_pause_and_invalid_modes_zero(self):
        p = self.nodes['power_constraints']['parameters']
        for mode in [0, -1, 4, 1.5]:
            for index in p['activeInputIndexes'] + p['reactiveInputIndexes']:
                self.assertEqual(self.evaluate(index, self.boundary(mode=mode)), 0)
            self.assertEqual(self.evaluate(m.APPLIED, self.boundary(mode=mode)), 0)

    def test_manual_mixed_sign_and_limits(self):
        indexes = self.nodes['power_constraints']['parameters']['activeInputIndexes']
        for index, expected in zip(indexes, [2, -3, 4]):
            self.assertEqual(self.evaluate(index, self.boundary(mode=2, soc=50)), expected)
        for invalid in [41.7, -41.7]:
            for index in indexes:
                values = self.boundary(mode=2, soc=50)
                values[m.MANUAL[0]] = invalid
                self.assertEqual(self.evaluate(index, values), 0)

    def test_manual_charge_inhibit_and_soc(self):
        indexes = self.nodes['power_constraints']['parameters']['activeInputIndexes']
        for index, expected in zip(indexes, [0, -3, 0]):
            self.assertEqual(self.evaluate(index, self.boundary(mode=2, soc=95)), expected)
            values = self.boundary(mode=2, soc=50, charge_command=0)
            values[1279] = 4369
            self.assertEqual(self.evaluate(index, values), expected)

    def test_reserve_uses_true_soc_and_full_limit(self):
        p = self.nodes['power_constraints']['parameters']
        self.assertEqual(self.evaluate(p['stateIndex'], self.boundary()), 99.5)
        self.assertEqual(self.evaluate(p['stateUpperIndex'], self.boundary()), 100)
        for index in p['activeInputIndexes']:
            self.assertEqual(self.evaluate(index, self.boundary()), 1)

    def test_full_charge_inhibit_keeps_standby_path(self):
        values = self.boundary(soc=100, charge_command=0)
        values[1279] = 4369
        safety = self.nodes['grid_reserve_controller']['parameters']['safetyPermitIndex']
        self.assertEqual(self.evaluate(safety, dict(values)), 1)
        for name in ['cycle_start', 'cycle_stop_clear', 'cycle_phase_control_mode']:
            index = self.nodes[name]['parameters']['permitIndex']
            self.assertEqual(self.evaluate(index, dict(values)), 1)
            denied = dict(values)
            denied[self.nodes['grid_reserve_controller']['parameters']['standbyOutput']] = 0
            self.assertEqual(self.evaluate(index, denied), 0)
        stop_conditions = self.nodes['pcs_energy_stop_permit']['parameters']['conditions']
        self.assertEqual(self.evaluate(stop_conditions[-1]['index'], dict(values)), 0)
        self.assertEqual(stop_conditions[-1]['value'], 1)

    def test_faults_block_device_safety(self):
        safety = self.nodes['grid_reserve_controller']['parameters']['safetyPermitIndex']
        for index, value in [(3999, 0), (1399, 0), (1212, 1), (1215, 1),
                             (1462, 1), (8199, 1), (1279, 43690)]:
            values = self.boundary()
            values[index] = value
            self.assertEqual(self.evaluate(safety, values), 0)

    def test_retained_stop_coil_does_not_block_startup(self):
        values = self.boundary()
        values.update({1202: 1, 1211: 0})
        safety = self.nodes['grid_reserve_controller']['parameters']['safetyPermitIndex']
        self.assertEqual(self.evaluate(safety, dict(values)), 1)
        for name in ['cycle_start', 'cycle_stop_clear']:
            self.assertEqual(self.evaluate(self.nodes[name]['parameters']['permitIndex'], dict(values)), 1)
        latch = self.nodes['grid_reserve_stop_latch']['parameters']
        self.assertTrue(all(c['index'] == m.MODE for t in latch['transitions'] for c in t['conditions']))
        values[m.MODE] = 0
        values[self.nodes['grid_reserve_controller']['parameters']['standbyOutput']] = 0
        self.assertEqual(self.evaluate(self.nodes['cycle_start']['parameters']['permitIndex'], values), 0)

    def test_no_new_hardware_writer_or_ownership_change(self):
        for node in self.graph['nodes']:
            if node['type'] != 'controlWrite':
                continue
            before = self.original[node['id']]['parameters']
            after = node['parameters']
            self.assertEqual({k: v for k, v in before.items() if k != 'permitIndex'},
                             {k: v for k, v in after.items() if k != 'permitIndex'})
        for i, index in enumerate(m.FINAL):
            values = {self.nodes[f'cycle_safe_p{i}']['parameters']['outputIndex']: 0, m.MODE: 3}
            self.assertEqual(self.evaluate(index, values), 0)

    def test_ports_and_graph_dag(self):
        ids = set(self.nodes)
        edges = {i: set() for i in ids}
        for link in self.graph['links']:
            self.assertIn(link['fromNodeId'], ids)
            self.assertIn(link['toNodeId'], ids)
            edges[link['fromNodeId']].add(link['toNodeId'])
        visiting, done = set(), set()
        def visit(i):
            self.assertNotIn(i, visiting, 'Dependency cycle: ' + i)
            if i in done:
                return
            visiting.add(i)
            for child in edges[i]:
                visit(child)
            visiting.remove(i)
            done.add(i)
        for i in ids:
            visit(i)
        for node in self.graph['nodes']:
            for port in node['ports']:
                value = node['parameters']
                for part in port['runtimePath'].strip('/').split('/'):
                    value = value[int(part)] if isinstance(value, list) else value[part]
                if port.get('binding', {}).get('kind') == 'point':
                    self.assertEqual(value, port['binding']['index'])

    def test_prepare_fragments_and_failures(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            report = m.prepare(RUNTIME, GRAPH, root/'candidate', 41.6, initial_mode=0)
            self.assertFalse(report['productionCurrentVerified'])
            points = m.read(root/'candidate/grid-reserve.points.json')
            by_index = {p['index']: p for p in points}
            self.assertEqual(by_index[m.MODE]['initialValue'], 0)
            self.assertEqual(by_index[m.PHASE_POWER]['initialValue'], 1)
            self.assertTrue(all(by_index[i]['initialValue'] == 0 for i in m.MANUAL))
            self.assertTrue(all(not by_index[i]['write']['enable'] for i in [m.STATUS, m.APPLIED, *m.FINAL]))
            with self.assertRaises(ValueError):
                m.prepare(RUNTIME, GRAPH, root/'candidate', 41.6, initial_mode=0)
            altered = root/'altered.json'
            altered.write_bytes(GRAPH.read_bytes() + b' ')
            with self.assertRaisesRegex(ValueError, 'SHA256'):
                m.prepare(RUNTIME, altered, root/'bad', 41.6, initial_mode=0)
            self.assertFalse((root/'bad').exists())

    def test_bad_rating_collision_and_wrong_feedback(self):
        for rating in [0, .5, -1, float('nan'), float('inf'), 41.7]:
            with self.assertRaises(ValueError):
                m.migrate_graph(self.source, rating)
        graph = copy.deepcopy(self.source)
        graph['nodes'][0]['parameters']['testIndex'] = m.MODE
        with self.assertRaisesRegex(ValueError, 'collide'):
            m.migrate_graph(graph, 41.6)
        devices = [m.read(p) for p in sorted((RUNTIME/'devices').glob('*.json'))]
        points = m.validate_points(devices)
        points[1216]['read']['enable'] = False
        with self.assertRaisesRegex(ValueError, 'feedback'):
            m.validate_points(devices)

    def test_stop_coil_mapping_must_be_verified(self):
        original = [m.read(p) for p in sorted((RUNTIME/'devices').glob('*.json'))]
        for access, key, value in [(None, 'address', 4), (None, 'enabled', False),
                ('read', 'enable', False), ('write', 'enable', False),
                ('read', 'function', 2), ('write', 'function', 6),
                ('read', 'address', 4), ('write', 'address', 4),
                ('read', 'dataType', 'uint16'), ('write', 'dataType', 'uint16'),
                ('read', 'scale', .1), ('write', 'offset', 1), ('write', 'length', 2)]:
            with self.subTest(access=access, key=key, value=value):
                devices = copy.deepcopy(original)
                stop = m.validate_points(devices)[1202]
                (stop if access is None else stop[access])[key] = value
                with self.assertRaisesRegex(ValueError, 'stop coil'):
                    m.validate_points(devices)


CURRENT = Path(__file__).resolve().parents[5]/'artifacts/comm104-deploy-20260912/snapshot-095831/running/config/runtime'


@unittest.skipUnless(CURRENT.is_dir(), 'Current captured runtime unavailable')
class CurrentSnapshotTests(unittest.TestCase):
    def test_current_snapshot_incremental_preservation(self):
        source_graph = CURRENT/m.CURRENT_GRAPH
        original = m.read(source_graph)
        self.assertEqual(m.digest(source_graph), m.CURRENT_SHA256)
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp)/'candidate'
            report = m.prepare(CURRENT, source_graph, output, 41.6, initial_mode=1)
            self.assertEqual(report['initialMode'], 1)
            self.assertEqual(report['activeRuleCode'], 'charge_discharge_cycle_8kw')
            self.assertEqual(report['activeRuleIntervalMs'], 1000)
            self.assertEqual(report['activeGraphProfile']['CHARGE_DISCHARGE_TEST'], '1')
            self.assertEqual(len(original['nodes']), 557)
            result = m.read(output/'runtime'/m.CURRENT_GRAPH)
            self.assertEqual(len(result['nodes']), 605)
            changed = set(report['modifiedOriginalNodeIds'])
            self.assertEqual(changed, {'power_constraints', 'pcs_energy_stop_permit',
                'cycle_start_hold_sequence', 'cycle_start_hold_value', 'cycle_phase_control_mode',
                'cycle_stop_clear', 'cycle_start', 'cycle_runtime_safety_gate'})
            nodes = {n['id']: n for n in result['nodes']}
            for node in original['nodes']:
                if node['id'] not in changed:
                    self.assertEqual(node, nodes[node['id']])
            self.assertEqual(nodes['cycle_phase_sequence']['parameters']['transitions'][0]['conditions'][0]['index'], 1569)
            for path in CURRENT.rglob('*'):
                if not path.is_file():
                    continue
                relative = path.relative_to(CURRENT)
                if relative.as_posix() not in {m.CURRENT_GRAPH, 'devices/device_ems_modular_virtual.json'}:
                    self.assertEqual(path.read_bytes(), (output/'runtime'/relative).read_bytes(), str(relative))
            before = m.read(CURRENT/'devices/device_ems_modular_virtual.json')
            after = m.read(output/'runtime/devices/device_ems_modular_virtual.json')
            old_meter = next(x for x in before['meters'] if x['meterCode'] == 'EMS_CORE')
            new_meter = next(x for x in after['meters'] if x['meterCode'] == 'EMS_CORE')
            self.assertEqual(old_meter['points'], new_meter['points'][:len(old_meter['points'])])
            mode = next(p for p in new_meter['points'] if p['index'] == m.MODE)
            self.assertEqual(mode['initialValue'], 1)
            self.assertFalse((output/'runtime/logic/shuntong_ems_graph.json').exists())

    def test_current_requires_explicit_nonactivating_mode(self):
        with tempfile.TemporaryDirectory() as temp:
            for mode in [None, 2, 3, True]:
                with self.subTest(mode=mode), self.assertRaises(ValueError):
                    m.prepare(CURRENT, CURRENT/m.CURRENT_GRAPH, Path(temp)/'invalid', 41.6, initial_mode=mode)
            self.assertFalse((Path(temp)/'invalid').exists())

    def test_changed_rule_profile_or_other_graph_collisions_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            source = Path(temp)/'source'
            shutil.copytree(CURRENT, source)
            app_path = source/'apps/mqtt-service.json'
            original = m.read(app_path)
            for mutate in [lambda c: c.update(enabled=False),
                    lambda c: c['rules'][0].update(enabled=False),
                    lambda c: c['rules'][0]['script']['graphProfile'].update(CHARGE_DISCHARGE_TEST='0'),
                    lambda c: c['rules'][0]['trigger'].update(intervalMs=10000)]:
                app = copy.deepcopy(original)
                mutate(app['computeEngine'])
                m.write(app_path, app)
                with self.assertRaises(ValueError):
                    m.current_rule(source, source/m.CURRENT_GRAPH)
            m.write(app_path, original)
            m.write(source/'logic/other.json', {'nodes': [{'outputIndex': m.MODE}]})
            with self.assertRaisesRegex(ValueError, 'collision'):
                m.current_rule(source, source/m.CURRENT_GRAPH)


if __name__ == '__main__':
    unittest.main()
