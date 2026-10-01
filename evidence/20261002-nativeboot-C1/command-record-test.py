"""Actual C recorder AST, private files/separate processes, no manager/root.

Windows does not prove POSIX O_NOFOLLOW; only that flag is zero-substituted.
Manager subprocess itself is a stub; no SSH/product/WSL/native execution.
"""
import ast
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import types
import unittest
import hashlib

HERE = Path(__file__).resolve().parent


def recorder(root):
    source = ast.parse((HERE / 'manager-child.py').read_text())
    functions = [n for n in source.body if isinstance(n, ast.FunctionDef) and
                 n.name in ('need', 'write', 'publish_record', 'reserve_command', 'collect_commands')]
    original = next(n for n in source.body if isinstance(n, ast.ClassDef) and n.name == 'ManagerChecks')
    run = next(n for n in original.body if isinstance(n, ast.FunctionDef) and n.name == 'run')
    cls = ast.ClassDef(name='Recorder', bases=[], keywords=[], body=[run], decorator_list=[])
    tree = ast.fix_missing_locations(ast.Module(body=functions + [cls], type_ignores=[]))
    posix = types.SimpleNamespace(**{name: getattr(os, name) for name in
        ('open', 'close', 'fdopen', 'getpid', 'fsync', 'rename', 'O_WRONLY', 'O_CREAT', 'O_EXCL')})
    posix.O_NOFOLLOW = getattr(os, 'O_NOFOLLOW', 0)
    scope = dict(os=posix, json=json, hashlib=hashlib, ROOT=root.parent, subprocess=subprocess)
    exec(compile(tree, 'actual-c1-recorder-AST', 'exec'), scope)
    instance = scope['Recorder'].__new__(scope['Recorder'])
    instance.logs = root; instance.commands = []; instance.units = ['c1-record-test.service']
    instance.unit_starts = {}
    instance.original_run = lambda command, **kw: subprocess.CompletedProcess(command, 0, b'private response\n', b'')
    return instance, scope


def child(directory, mode):
    obj, scope = recorder(Path(directory))
    if mode == 'command': obj.run(['/usr/bin/systemctl', 'show', 'c1-record-test.service'])
    elif mode == 'dispatch': obj.run(['/usr/bin/systemd-run', '--unit=c1-record-test.service'])
    elif mode == 'reserve': scope['reserve_command'](Path(directory), ['/usr/bin/systemctl', 'show', 'c1-record-test.service'])
    else: raise ValueError(mode)


class Records(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix='c1-record-local-')
        self.addCleanup(temp.cleanup); self.root = Path(temp.name)
        self.obj, self.scope = recorder(self.root)

    def spawn(self, mode, expected=0):
        result = subprocess.run([sys.executable, '-B', str(Path(__file__).resolve()), '--child', str(self.root), mode],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=15)
        self.assertEqual(result.returncode, expected, result.stderr.decode())
        return result

    def command(self):
        self.obj.run(['/usr/bin/systemctl', 'show', 'c1-record-test.service'])

    def test_parent_child_parent_logs_unique_and_parent_merges(self):
        self.command(); self.spawn('command'); self.command()
        rows = self.scope['collect_commands'](self.root)
        self.assertEqual([r['index'] for r in rows], [0, 1, 2])
        self.assertEqual(rows[0]['pid'], rows[2]['pid'])
        self.assertNotEqual(rows[0]['pid'], rows[1]['pid'])
        self.assertEqual(len(self.obj.commands), 3)
        self.assertEqual(len(list(self.root.glob('*.log'))), 3)

    def test_child_dispatch_merged_and_no_parent_duplicate(self):
        self.spawn('dispatch')
        self.command()
        self.assertEqual(self.obj.unit_starts, {'c1-record-test.service': 1})
        with self.assertRaises(FileExistsError):
            self.obj.run(['/usr/bin/systemd-run', '--unit=c1-record-test.service'])
        self.assertEqual(len(list(self.root.glob('*.request.json'))), 2)

    def test_global160_budget_spans_processes(self):
        for n in range(159): self.scope['reserve_command'](self.root, ['/usr/bin/systemctl', 'show', 'c1-record-test.service'])
        self.spawn('reserve')
        with self.assertRaisesRegex(ValueError, 'global manager command budget'):
            self.command()
        self.assertEqual(len(list(self.root.glob('*.request.json'))), 160)

    def test_unknown_child_reservation_not_hidden(self):
        self.spawn('reserve'); self.command()
        rows = self.scope['collect_commands'](self.root)
        self.assertEqual(rows[0]['response']['outcome'], 'UNKNOWN')
        self.assertEqual(rows[1]['response']['outcome'], 'RETURNED')

    def test_timeout_preserves_partial_log_and_unknown_request(self):
        def failure(command, **kwargs):
            raise subprocess.TimeoutExpired(command, 15, output=b'partial-out', stderr=b'partial-err')
        self.obj.original_run = failure
        with self.assertRaises(subprocess.TimeoutExpired): self.command()
        row = self.scope['collect_commands'](self.root)[0]
        self.assertEqual(row['response']['outcome'], 'UNKNOWN')
        self.assertIn(b'partial-out', (self.root / '000.log').read_bytes())
        self.assertIn(b'partial-err', (self.root / '000.log').read_bytes())

    def test_unknown_dispatch_is_not_replayed(self):
        def failure(command, **kwargs): raise OSError('submission outcome unknown')
        self.obj.original_run = failure
        with self.assertRaises(OSError):
            self.obj.run(['/usr/bin/systemd-run', '--unit=c1-record-test.service'])
        with self.assertRaises(FileExistsError):
            self.obj.run(['/usr/bin/systemd-run', '--unit=c1-record-test.service'])
        self.assertEqual(len(list(self.root.glob('*.request.json'))), 1)


if __name__ == '__main__':
    if len(sys.argv) == 4 and sys.argv[1] == '--child':
        child(sys.argv[2], sys.argv[3])
    else:
        unittest.main(verbosity=2)
