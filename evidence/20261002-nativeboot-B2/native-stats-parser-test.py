"""Pure actual B2 validator AST regression; never import/run ARM payload or SSH."""
import ast
import hashlib
import json
from pathlib import Path
import re
import unittest

HERE = Path(__file__).resolve().parent


class StatsContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        raw = (HERE.parent / '20261002-nativeboot-B1/board-23-B1-result.json').read_bytes()
        assert hashlib.sha256(raw).hexdigest() == '20686442532f51ebf95c8cfffecc7dd1abd6d8814b566f0048df5e34ce4bd705'
        record = json.loads(raw)
        command = record['execution']['nativeResult']['commands'][0]
        cls.original_name = command['argv'][-1]
        cls.out, cls.err, cls.code = command['stdout'].encode('ascii'), command['stderr'].encode('ascii'), command['exitCode']
        tree = ast.parse((HERE / 'native-child.py').read_bytes(), feature_version=(3, 8))
        nodes = [node for node in tree.body if
                 isinstance(node, ast.Assign) and any(isinstance(t, ast.Name) and t.id == 'PHYSICAL_SLOTS' for t in node.targets)
                 or isinstance(node, ast.FunctionDef) and node.name in ('need', 'validate_stats')]
        assert len(nodes) == 3
        namespace = {'re': re}
        exec(compile(ast.Module(body=nodes, type_ignores=[]), str(HERE / 'native-child.py'), 'exec'), namespace)
        cls.parser = staticmethod(namespace['validate_stats'])
        cls.physical = namespace['PHYSICAL_SLOTS']

    def reject(self, out=None, code=None, err=None, name=None, message='native empty queue/stats proof failed'):
        with self.assertRaisesRegex(ValueError, '^' + re.escape(message) + '$'):
            self.parser(name if name is not None else self.original_name,
                        self.code if code is None else code, self.out if out is None else out,
                        self.err if err is None else err)

    def test_original_raw_native_stdout_exact_success(self):
        self.assertIsNone(self.parser(self.original_name, self.code, self.out, self.err))

    def test_two_fixed_new_names_exact_success(self):
        for name in ('gw_native_b2_a9b2d7d_cold', 'gw_native_b2_a9b2d7d_interrupt'):
            with self.subTest(name=name):
                out = self.out.replace(self.original_name.encode(), name.encode())
                self.assertIsNone(self.parser(name, 0, out, b''))

    def test_usable_capacity_is_not_physical_ring_slots(self):
        self.assertEqual(self.physical, {'latest':100000, 'pending':4096, 'persistent':20000, 'updates':65536})
        for before, after in ((b'latestCapacity=100000', b'latestCapacity=99999'),
                              (b'pendingCapacity=4095', b'pendingCapacity=4096'),
                              (b'persistentCapacity=19999', b'persistentCapacity=20000'),
                              (b'pointUpdates=0/65535', b'pointUpdates=0/65536')):
            with self.subTest(field=before): self.reject(self.out.replace(before, after))

    def test_configured_limits_do_not_accept_other_digits(self):
        for before, after in ((b'latest=0/100000', b'latest=0/99999'),
                              (b'pendingWrites=0/4096', b'pendingWrites=0/4095'),
                              (b'persistent=0/20000', b'persistent=0/19999'),
                              (b'pointUpdates=0/65535', b'pointUpdates=0/12345')):
            with self.subTest(field=before): self.reject(self.out.replace(before, after))

    def test_every_nonempty_queue_refuses(self):
        for field in (b'latest', b'pendingWrites', b'persistent', b'pointUpdates'):
            with self.subTest(field=field): self.reject(self.out.replace(field + b'=0/', field + b'=1/'))

    def test_every_nonzero_sequence_refuses(self):
        for field in (b'writeSeq', b'persistentSeq', b'pointUpdateSeq'):
            with self.subTest(field=field): self.reject(self.out.replace(field + b'=0', field + b'=1'))

    def test_wrong_name_or_posix_normalization_refuses(self):
        for out in (self.out.replace(self.original_name.encode(), b'other_name'),
                    self.out.replace(b'shm=/', b'shm='), self.out.replace(b'shm=/', b'shm=//')):
            with self.subTest(out=out): self.reject(out)
        self.reject(name='../escape', message='invalid expected stats name')

    def test_failure_signal_and_wait_status_refuse(self):
        for code in (1, -9, 256):
            with self.subTest(code=code): self.reject(code=code, message='native stats exit mismatch')
        self.reject(code=True, message='invalid stats exit contract')

    def test_success_stderr_refuses(self):
        self.reject(err=b'warning\n')

    def test_format_extra_bytes_or_nonbytes_refuse(self):
        for out in (self.out.rstrip(b'\n'), self.out + b'\n', self.out + b'ignored=1\n',
                    self.out.replace(b'latestCapacity=100000 ', b'')):
            with self.subTest(out=out): self.reject(out)
        self.reject(out=self.out.decode(), message='stats output must be bytes')

    def test_exact_private_size_refusal_succeeds(self):
        name = 'gw_native_b2_a9b2d7d_interrupt'
        err = ('pointctl error: shared memory size mismatch: /' + name + '\n').encode()
        self.assertIsNone(self.parser(name, 1, b'', err, 1))

    def test_private_size_refusal_does_not_swallow_other_errors(self):
        name = 'gw_native_b2_a9b2d7d_interrupt'
        exact = ('pointctl error: shared memory size mismatch: /' + name + '\n').encode()
        for out, err in ((b'partial output', exact), (b'', exact.replace(name.encode(), b'wrong_name')),
                         (b'', exact.replace(b'size mismatch', b'version mismatch')), (b'', exact + b'extra\n')):
            with self.subTest(out=out, err=err), self.assertRaisesRegex(ValueError, '^wrong-size refusal not exact$'):
                self.parser(name, 1, out, err, 1)
        with self.assertRaisesRegex(ValueError, '^native stats exit mismatch$'): self.parser(name, 0, b'', exact, 1)
        with self.assertRaisesRegex(ValueError, '^invalid stats exit contract$'): self.parser(name, 2, b'', exact, 2)


if __name__ == '__main__':
    unittest.main(verbosity=2)
