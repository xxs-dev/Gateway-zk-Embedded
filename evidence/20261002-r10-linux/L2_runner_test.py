"""Windows private-file/API-model regression; NOT Linux fork/namespace proof."""
import contextlib
import ctypes
import importlib.util
import io
import json
import stat
from pathlib import Path
import subprocess
import tempfile
import types
import unittest
from unittest import mock
from paramiko import SFTPAttributes

HERE = Path(__file__).resolve().parent


def load(name, filename):
    spec = importlib.util.spec_from_file_location(name, HERE / filename)
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
    return module


builder = load('L2_builder', 'build-child.py')
environment = load('L2_environment', 'build-environment.py')
launcher = load('L2_launcher', 'build-group.py')
runner = load('L2_runner', 'execute-r10.py')


class SFTPPolicyTests(unittest.TestCase):
    def attributes(self, mode=stat.S_IFDIR | 0o755, uid=0, gid=0):
        info = SFTPAttributes()
        info.st_mode, info.st_uid, info.st_gid = mode, uid, gid
        self.assertFalse(hasattr(info, 'st_dev'))
        self.assertFalse(hasattr(info, 'st_ino'))
        return info

    def test_standard_v3_attributes_need_no_dev_inode(self):
        runner.check_sftp_parent(self.attributes(), dict(uid=0, gid=0, mode=0o755, dev=12, ino=34))

    def test_symlink_owner_and_mode_drift_rejected(self):
        expected = dict(uid=0, gid=0, mode=0o755, dev=12, ino=34)
        for values in ({'mode': stat.S_IFLNK | 0o755}, {'uid': 1000}, {'gid': 1000},
                       {'mode': stat.S_IFDIR | 0o777}):
            with self.subTest(values=values), self.assertRaises(AssertionError):
                runner.check_sftp_parent(self.attributes(**values), expected)


class ReceiptTests(unittest.TestCase):
    def first(self):
        result = dict(phase='STARTED')
        try: raise ValueError('FIRST')
        except ValueError as error: builder.primary_error(result, error)
        return result

    def test_first_error_saved_before_finally_error(self):
        with tempfile.TemporaryDirectory(prefix='r10-L2-private-') as name, mock.patch.object(builder, 'sync_directory'):
            root = Path(name); result = self.first()
            builder.persist(root, result); first = json.loads((root / 'build-result.json').read_text())['primaryError']
            builder.finish(result, mock.Mock(side_effect=PermissionError('POST')), lambda: builder.persist(root, result))
            saved = json.loads((root / 'build-result.json').read_text())
            self.assertEqual(saved['primaryError'], first)
            self.assertEqual(saved['postcheckErrors'][0]['type'], 'PermissionError')

    def test_save_failure_does_not_replace_first_error(self):
        result = self.first(); first = dict(result['primaryError'])
        builder.finish(result, mock.Mock(side_effect=RuntimeError('POST')), mock.Mock(side_effect=OSError('SAVE')))
        self.assertEqual(result['primaryError'], first)
        self.assertEqual(len(result['postcheckErrors']), 1)
        self.assertEqual(result['receiptErrors'][0]['message'], 'SAVE')

    def test_later_primary_does_not_replace_first(self):
        result = self.first(); first = dict(result['primaryError'])
        try: raise RuntimeError('SECOND')
        except RuntimeError as error: builder.primary_error(result, error)
        self.assertEqual(result['primaryError'], first)

    def test_failed_replace_preserves_previous_receipt(self):
        with tempfile.TemporaryDirectory(prefix='r10-L2-private-') as name, mock.patch.object(builder, 'sync_directory'):
            root = Path(name); builder.persist(root, dict(phase='STARTED'))
            before = (root / 'build-result.json').read_bytes()
            with mock.patch.object(builder.os, 'replace', side_effect=OSError('RENAME')):
                with self.assertRaises(OSError): builder.persist(root, self.first())
            self.assertEqual((root / 'build-result.json').read_bytes(), before)
            self.assertEqual(len(list(root.glob('.receipt-*'))), 1)  # Kept, not blindly removed.

    def test_directory_sync_error_keeps_published_first_error(self):
        with tempfile.TemporaryDirectory(prefix='r10-L2-private-') as name:
            root = Path(name); result = self.first()
            with mock.patch.object(builder, 'sync_directory', side_effect=OSError('DIRSYNC')):
                with self.assertRaises(OSError): builder.persist(root, result)
            self.assertEqual(json.loads((root / 'build-result.json').read_text())['primaryError'], result['primaryError'])


class GitPolicyTests(unittest.TestCase):
    def test_fixed_git_env_and_owner_drop_order(self):
        owner = [{'uid': 1000, 'gid': 1000}]; calls = []
        def run(argv, **kwargs):
            self.assertEqual(argv[0], '/usr/bin/git')
            self.assertEqual(kwargs['env']['GIT_OPTIONAL_LOCKS'], '0')
            self.assertEqual(kwargs['env']['PATH'], '/usr/bin:/bin')
            self.assertIn('core.fsmonitor=false', argv); self.assertIn('core.hooksPath=/dev/null', argv)
            self.assertNotIn('safe.directory', ' '.join(argv))
            kwargs['preexec_fn']()
            return subprocess.CompletedProcess(argv, 0, b'head\n', b'')
        with mock.patch.object(environment, 'owner_identity', return_value=owner), mock.patch.object(environment.subprocess, 'run', side_effect=run), \
             mock.patch.object(environment.os, 'setgroups', side_effect=lambda value: calls.append(('groups', value)), create=True), \
             mock.patch.object(environment.os, 'setgid', side_effect=lambda value: calls.append(('gid', value)), create=True), \
             mock.patch.object(environment.os, 'setuid', side_effect=lambda value: calls.append(('uid', value)), create=True), \
             mock.patch.object(environment.os, 'getuid', return_value=1000, create=True), mock.patch.object(environment.os, 'geteuid', return_value=1000, create=True):
            environment.git_read(['rev-parse', 'HEAD'], owner)
        self.assertEqual(calls, [('groups', []), ('gid', 1000), ('uid', 1000)])

    def test_owner_drift_refuses_before_command(self):
        with mock.patch.object(environment, 'owner_identity', return_value=[]), mock.patch.object(environment.subprocess, 'run') as run:
            with self.assertRaises(AssertionError): environment.git_read(['rev-parse', 'HEAD'], [{'uid': 1000}])
            run.assert_not_called()

    def test_arbitrary_git_command_refuses(self):
        with mock.patch.object(environment, 'owner_identity', return_value=[]), mock.patch.object(environment.subprocess, 'run') as run:
            with self.assertRaises(AssertionError): environment.git_read(['config', '--global', 'safe.directory', '*'], [])
            run.assert_not_called()


class ExecBoundary(BaseException):
    pass


class FakeLibc:
    def prctl(self, operation, *args):
        if operation == 2: args[0]._obj.value = 9
        return 0
    def unshare(self, flags):
        assert flags == 0x20000000
        return 0


class LauncherTests(unittest.TestCase):
    def scenario(self, parents, incoming='GO\n', fork=0):
        stack = contextlib.ExitStack(); self.addCleanup(stack.close)
        stack.enter_context(mock.patch.object(launcher.ctypes, 'CDLL', return_value=FakeLibc()))
        stack.enter_context(mock.patch.object(launcher, 'host_parent', side_effect=parents))
        stack.enter_context(mock.patch.object(launcher.sys, 'argv', ['launcher', '["/bin/false"]', '2', '123']))
        stack.enter_context(mock.patch.object(launcher.sys, 'stdin', io.StringIO(incoming)))
        stack.enter_context(mock.patch.object(launcher.os, 'getpid', side_effect=[77, 1] if fork == 0 else [77]))
        stack.enter_context(mock.patch.object(launcher.os, 'fork', return_value=fork, create=True))
        stack.enter_context(mock.patch.object(launcher.os, 'readlink', return_value='pid:[model]'))
        stack.enter_context(mock.patch.object(launcher.signal, 'signal'))
        stack.enter_context(mock.patch.object(launcher.signal, 'alarm', create=True))
        stack.enter_context(mock.patch.object(launcher.signal, 'SIGKILL', 9, create=True))
        stack.enter_context(mock.patch.object(launcher.signal, 'SIGALRM', 14, create=True))
        stack.enter_context(contextlib.redirect_stdout(io.StringIO()))
        return stack.enter_context(mock.patch.object(launcher.os, 'execvpe', side_effect=ExecBoundary))

    def test_child_checks_parent_before_go_exec(self):
        execute = self.scenario([123, 77])
        with self.assertRaises(ExecBoundary): launcher.main()
        execute.assert_called_once()

    def test_parent_lost_in_fork_setup_never_executes(self):
        execute = self.scenario([123, 1])
        with self.assertRaises(AssertionError): launcher.main()
        execute.assert_not_called()

    def test_missing_go_never_executes(self):
        execute = self.scenario([123, 77], incoming='')
        with self.assertRaises(AssertionError): launcher.main()
        execute.assert_not_called()

    def test_parent_leaves_pid_unreaped_through_final_signal(self):
        self.scenario([123], fork=42)
        info = types.SimpleNamespace(si_status=0, si_code=1)
        constants = {'P_PID': 1, 'WEXITED': 4, 'WNOHANG': 1, 'WNOWAIT': 0x1000000, 'CLD_EXITED': 1}
        with contextlib.ExitStack() as stack:
            for name, value in constants.items(): stack.enter_context(mock.patch.object(launcher.os, name, value, create=True))
            waitid = stack.enter_context(mock.patch.object(launcher.os, 'waitid', return_value=info, create=True))
            waitpid = stack.enter_context(mock.patch.object(launcher.os, 'waitpid', create=True))
            kill = stack.enter_context(mock.patch.object(launcher.os, 'kill'))
            self.assertEqual(launcher.main(), 0)
            self.assertTrue(waitid.call_args.args[2] & constants['WNOWAIT'])
            waitpid.assert_not_called(); kill.assert_called_once_with(42, 9)


if __name__ == '__main__':
    unittest.main(verbosity=2)
