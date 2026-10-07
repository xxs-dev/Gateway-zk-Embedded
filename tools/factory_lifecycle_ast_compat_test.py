"""Policy staging regressions. Legacy AST is simulated, not Python 3.6 execution."""
import ast
import importlib.util
from pathlib import Path
import types
import unittest

REPO = Path(__file__).resolve().parents[1]


def stage_module(mode='native'):
    spec = importlib.util.spec_from_file_location('factory_policy_compat', REPO / 'deploy/factory-lifecycle-stage.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    if mode == 'native':
        return module

    class LegacyBool(ast.expr):
        _fields = ('value',)
        _attributes = ('lineno', 'col_offset')

    class LegacyLocations(ast.NodeTransformer):
        def visit_Constant(self, node):
            if mode == 'legacy' and type(node.value) is bool:
                replacement = LegacyBool()
                replacement.value = node.value
                replacement.lineno, replacement.col_offset = node.lineno, node.col_offset
                return replacement
            return node

    def parse(raw):
        tree = LegacyLocations().visit(ast.parse(raw))
        for node in ast.walk(tree):
            for name in ('end_lineno', 'end_col_offset'):
                if getattr(node, name, None) is not None:
                    delattr(node, name)
        return tree

    facade = types.SimpleNamespace(**{name: getattr(ast, name) for name in dir(ast) if not name.startswith('_')})
    facade.parse = parse
    facade.NameConstant = LegacyBool
    module.ast = facade
    return module


class PolicyCompat(unittest.TestCase):
    def assert_projection(self, mode):
        module = stage_module(mode)
        cases = [
            (b'CORE_TEST_APPLY_ALLOWED = True\n', b'CORE_TEST_APPLY_ALLOWED = False\n'),
            (b'CORE_TEST_APPLY_ALLOWED = False\n', b'CORE_TEST_APPLY_ALLOWED = False\n'),
            (b'# True stays\nCORE_TEST_APPLY_ALLOWED=( # comment True\n True\n); kept=True\n',
             b'# True stays\nCORE_TEST_APPLY_ALLOWED=( # comment True\n False\n); kept=True\n'),
            ('label="\u4e2d\u6587"; CORE_TEST_APPLY_ALLOWED=True; kept=True # True\n'.encode('utf-8'),
             'label="\u4e2d\u6587"; CORE_TEST_APPLY_ALLOWED=False; kept=True # True\n'.encode('utf-8')),
            (b'CORE_TEST_APPLY_ALLOWED = (\r\n False # retained\r\n)\r\n',
             b'CORE_TEST_APPLY_ALLOWED = (\r\n False # retained\r\n)\r\n'),
        ]
        for raw, expected in cases:
            with self.subTest(mode=mode, raw=raw):
                staged = module.disabled_entry(raw)
                self.assertEqual(expected, staged)
                compile(staged, '<test-disabled>', 'exec')
                declaration = next(node for node in ast.parse(staged).body if isinstance(node, ast.Assign)
                    and isinstance(node.targets[0], ast.Name) and node.targets[0].id == module.POLICY)
                self.assertIs(declaration.value.value, False)

    def test_native_exact_literal_projection(self):
        self.assert_projection('native')

    def test_legacy_bool_without_end_positions(self):
        self.assert_projection('legacy')

    def test_constant_without_end_positions(self):
        self.assert_projection('locations')

    def test_all_modes_reject_other_bindings_dynamic_and_nonbool(self):
        cases = [
            b'CORE_TEST_APPLY_ALLOWED=True\nCORE_TEST_APPLY_ALLOWED=False\n',
            b'CORE_TEST_APPLY_ALLOWED=True\ndef f():\n CORE_TEST_APPLY_ALLOWED=False\n',
            b'CORE_TEST_APPLY_ALLOWED: bool=True\n', b'other=CORE_TEST_APPLY_ALLOWED=True\n',
            b'CORE_TEST_APPLY_ALLOWED=True\ndef CORE_TEST_APPLY_ALLOWED(): pass\n',
            b'CORE_TEST_APPLY_ALLOWED=True\nimport os as CORE_TEST_APPLY_ALLOWED\n',
            b'CORE_TEST_APPLY_ALLOWED=True\ndef f(CORE_TEST_APPLY_ALLOWED): pass\n',
        ] + [('CORE_TEST_APPLY_ALLOWED=' + expression + '\n').encode('ascii')
             for expression in ('bool(1)', '1', '0', 'None', '"True"', 'not False', 'True if x else False')]
        for mode in ('native', 'legacy', 'locations'):
            for raw in cases:
                with self.subTest(mode=mode, raw=raw), self.assertRaisesRegex(ValueError, 'policy assignment'):
                    stage_module(mode).disabled_entry(raw)

    def test_original_fixed_policy_stages_false_without_executing_owner(self):
        raw = (REPO / 'deploy/offline-runtime-upgrade.py').read_bytes()
        expected = stage_module().disabled_entry(raw)
        for mode in ('legacy', 'locations'):
            self.assertEqual(expected, stage_module(mode).disabled_entry(raw))


if __name__ == '__main__':
    unittest.main(verbosity=2)
