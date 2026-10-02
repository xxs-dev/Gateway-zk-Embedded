"""Private local interface read/errno tests; no Linux namespace evidence."""
import ast
import importlib.util
from pathlib import Path
import tempfile
import unittest

HERE=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('P2_probe',HERE/'proc-readonly.py')
probe=importlib.util.module_from_spec(spec);spec.loader.exec_module(probe)


class Tests(unittest.TestCase):
    def test_actual_missing_file_preserves_errno(self):
        with tempfile.TemporaryDirectory() as directory:
            result=probe.read_interface(str(Path(directory)/'absent'))
        self.assertFalse(result['readable']);self.assertEqual(result['errno'],2)

    def test_actual_private_bytes_preserved(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'children';path.write_bytes(b'12 34 ')
            result=probe.read_interface(str(path))
        self.assertTrue(result['readable']);self.assertEqual(result['raw'],'12 34 ')

    def test_oversized_interface_refuses(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'children';path.write_bytes(b'1'*65537)
            with self.assertRaises(AssertionError):probe.read_interface(str(path))

    def test_fixed_inventory_has_no_mutation_call(self):
        tree=ast.parse((HERE/'proc-readonly.py').read_text())
        prohibited={'kill','killpg','unlink','rmdir','rmtree','mkdir','mount','unshare','chroot','write_bytes','write_text','chmod','chown'}
        methods={n.func.attr for n in ast.walk(tree) if isinstance(n,ast.Call) and isinstance(n.func,ast.Attribute)}
        self.assertFalse(methods&prohibited)
        self.assertEqual(set(probe.TARGETS.values()),{'pid:[4026532696]','mnt:[4026532695]','net:[4026532698]'})

    def test_actual_private_directory_bound(self):
        with tempfile.TemporaryDirectory() as directory:
            for name in ('1','2','3'): (Path(directory)/name).write_bytes(b'')
            with self.assertRaises(AssertionError):probe.bounded_entries(Path(directory),2)
            self.assertEqual(len(probe.bounded_entries(Path(directory),3,numeric=True)),3)


if __name__=='__main__':unittest.main(verbosity=2)
