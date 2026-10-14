"""Run only against a reviewed binary in an explicitly private test tmpfs."""
import hashlib
import os
from pathlib import Path
import stat
import struct
import subprocess
import unittest
import uuid


@unittest.skipUnless(os.name == 'posix' and os.environ.get('GATEWAY_TEST_POINTCTL'),
                     'requires an explicit native pointctl and private SHM fixture')
class ExclusivePointctlTest(unittest.TestCase):
    def setUp(self):
        rows = [line.split() for line in Path('/proc/self/mountinfo').read_text().splitlines()]
        private = [r for r in rows if r[4] == '/dev/shm' and r[r.index('-') + 1] == 'tmpfs' and
                   r[r.index('-') + 2].startswith('gateway-pointctl-empty-test-')]
        self.assertEqual(len(private), 1, 'refusing nonfixture /dev/shm')
        self.assertNotEqual(os.readlink('/proc/self/ns/mnt'), os.readlink('/proc/1/ns/mnt'))
        self.binary = str(Path(os.environ['GATEWAY_TEST_POINTCTL']).resolve(strict=True))
        self.name = 'pointctl_empty_' + uuid.uuid4().hex
        self.path = Path('/dev/shm') / self.name
        self.extra = []
        self.addCleanup(self.cleanup)

    def cleanup(self):
        for path in [self.path] + self.extra:
            if path.is_symlink() or path.is_file():
                path.unlink()
            elif path.is_dir():
                path.rmdir()

    def run_cli(self, *args):
        return subprocess.run([self.binary] + list(args), stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              timeout=20, env={'PATH': '/usr/bin:/bin', 'LC_ALL': 'C'})

    def test_creates_native_empty_v11_with_private_permissions(self):
        result = self.run_cli('create-empty', '--shm', self.name)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(b'latest=0/', result.stdout)
        self.assertIn(b'pendingWrites=0/', result.stdout)
        self.assertIn(b'writeSeq=0 persistentSeq=0 pointUpdateSeq=0', result.stdout)
        self.assertEqual(self.path.stat().st_size, 15667032)
        self.assertEqual(stat.S_IMODE(self.path.stat().st_mode), 0o600)
        with self.path.open('rb') as stream:
            self.assertEqual(stream.read(8), struct.pack('<II', 0x4d505354, 11))

    def test_existing_initialized_store_is_byte_identical_after_refusal(self):
        self.assertEqual(self.run_cli('create-empty', '--shm', self.name).returncode, 0)
        before = self.path.stat().st_ino, hashlib.sha256(self.path.read_bytes()).hexdigest()
        result = self.run_cli('create-empty', '--shm', self.name)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b'exclusive shared memory create failed', result.stderr)
        self.assertEqual((self.path.stat().st_ino, hashlib.sha256(self.path.read_bytes()).hexdigest()), before)

    def test_existing_zero_partial_and_legacy_bytes_are_never_initialized(self):
        for raw in (b'', b'\0' * 8, b'old ABI10 evidence do not rewrite'):
            with self.subTest(size=len(raw)):
                self.path.write_bytes(raw)
                before = self.path.stat().st_ino
                self.assertNotEqual(self.run_cli('create-empty', '--shm', self.name).returncode, 0)
                self.assertEqual(self.path.read_bytes(), raw)
                self.assertEqual(self.path.stat().st_ino, before)
                self.path.unlink()

    def test_existing_symlink_hardlink_and_directory_are_not_followed(self):
        target = self.path.with_name(self.name + '_target')
        self.extra.append(target)
        target.write_bytes(b'foreign retained target')
        self.path.symlink_to(target)
        self.assertNotEqual(self.run_cli('create-empty', '--shm', self.name).returncode, 0)
        self.assertTrue(self.path.is_symlink())
        self.path.unlink()
        os.link(str(target), str(self.path))
        self.assertNotEqual(self.run_cli('create-empty', '--shm', self.name).returncode, 0)
        self.assertEqual(target.read_bytes(), b'foreign retained target')
        self.path.unlink()
        self.path.mkdir()
        self.assertNotEqual(self.run_cli('create-empty', '--shm', self.name).returncode, 0)
        self.assertTrue(self.path.is_dir())

    def test_invalid_options_have_no_implicit_or_config_store_side_effect(self):
        before = {p.name for p in Path('/dev/shm').iterdir()}
        for args in ((), ('--shm',), ('--shm', ''), ('--shm', '/bad'), ('--shm', '../bad'),
                     ('--shm', 'a' * 128), ('--shm', self.name, '--shm', self.name),
                     ('--shm', self.name, '--app-config', '/missing'), ('--app-config', '/missing')):
            with self.subTest(args=args):
                result = self.run_cli('create-empty', *args)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(b'requires exactly --shm', result.stderr)
                self.assertEqual({p.name for p in Path('/dev/shm').iterdir()}, before)

    def test_concurrent_creators_have_one_winner(self):
        args = [self.binary, 'create-empty', '--shm', self.name]
        processes = [subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                     env={'PATH': '/usr/bin:/bin', 'LC_ALL': 'C'}) for _ in range(2)]
        try:
            for process in processes:
                process.communicate(timeout=20)
            self.assertEqual(sorted(p.returncode for p in processes), [0, 1])
            with self.path.open('rb') as stream:
                self.assertEqual(stream.read(8), struct.pack('<II', 0x4d505354, 11))
        finally:
            for process in processes:
                if process.poll() is None:
                    process.kill()
                process.wait()


if __name__ == '__main__':
    unittest.main()
