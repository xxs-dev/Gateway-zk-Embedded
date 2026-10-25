"""deploy/gateway-tls-enroll.sh with real openssl and a synthetic CA (no platform, no network, no real devices).

The synthetic CA mirrors what the platform issues: root -> device CA (pathlen 0) -> leaf with keyUsage
digitalSignature and clientAuth (mqtt) or serverAuth (maintenance), CN = machine code.
Runs on Linux with dash when available (the strictest POSIX shell), otherwise sh.
"""
import os
from pathlib import Path
import shutil
import stat
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[1]
SCRIPT = REPO / 'deploy/gateway-tls-enroll.sh'
SHELL = shutil.which('dash') or 'sh'
MACHINE = 'GW_TEST_001'


def openssl(*args, cwd):
    subprocess.run(['openssl', *args], cwd=cwd, check=True, capture_output=True, timeout=30)


class Ca:
    """root.pem / device-ca.pem (+ keys) in a temp directory; signs device CSRs like the platform does."""

    def __init__(self, root, name='Synthetic'):
        self.dir = root
        self.dir.mkdir(parents=True, exist_ok=True)
        (self.dir / 'req.cnf').write_text('[req]\ndistinguished_name=dn\nprompt=no\n[dn]\n')
        (self.dir / 'ext.cnf').write_text(
            '[root]\nbasicConstraints=critical,CA:TRUE\nkeyUsage=critical,keyCertSign,cRLSign\n'
            'subjectKeyIdentifier=hash\n'
            '[device_ca]\nbasicConstraints=critical,CA:TRUE,pathlen:0\nkeyUsage=critical,keyCertSign,cRLSign\n'
            'subjectKeyIdentifier=hash\nauthorityKeyIdentifier=keyid\n'
            '[mqtt]\nbasicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature\nextendedKeyUsage=clientAuth\n'
            'authorityKeyIdentifier=keyid\n'
            '[maintenance]\nbasicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature\nextendedKeyUsage=serverAuth\n'
            'authorityKeyIdentifier=keyid\n')
        for key in ('root.key', 'device-ca.key'):
            openssl('genpkey', '-algorithm', 'EC', '-pkeyopt', 'ec_paramgen_curve:P-256', '-out', key, cwd=self.dir)
        openssl('req', '-x509', '-new', '-key', 'root.key', '-subj', f'/CN={name} Root', '-days', '30',
                '-config', 'req.cnf', '-addext', 'basicConstraints=critical,CA:TRUE',
                '-addext', 'keyUsage=critical,keyCertSign,cRLSign', '-out', 'root.pem', cwd=self.dir)
        openssl('req', '-new', '-key', 'device-ca.key', '-subj', f'/CN={name} Device CA', '-out', 'device-ca.csr',
                '-config', 'req.cnf', cwd=self.dir)
        openssl('x509', '-req', '-in', 'device-ca.csr', '-CA', 'root.pem', '-CAkey', 'root.key', '-set_serial', '2',
                '-days', '30', '-extfile', 'ext.cnf', '-extensions', 'device_ca', '-out', 'device-ca.pem', cwd=self.dir)
        self.serial = 100

    @property
    def bundle(self):
        return self.dir / 'root.pem'

    def sign(self, csr_pem, profile, out):
        """Writes leaf + device CA to `out` (what the platform's chainPem holds)."""
        self.serial += 1
        (self.dir / 'req.csr').write_text(csr_pem)
        openssl('x509', '-req', '-in', 'req.csr', '-CA', 'device-ca.pem', '-CAkey', 'device-ca.key',
                '-set_serial', str(self.serial), '-days', '7', '-extfile', 'ext.cnf', '-extensions', profile,
                '-out', 'leaf.pem', cwd=self.dir)
        out.write_text((self.dir / 'leaf.pem').read_text() + (self.dir / 'device-ca.pem').read_text())
        return out


class EnrollScriptTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix='gz-enroll-test-')
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.home = self.root / 'home'
        self.home.mkdir()
        self.ca = Ca(self.root / 'ca')
        self.chain = self.root / 'chain.pem'

    def run_script(self, *args):
        env = dict(os.environ, GATEWAY_HOME=str(self.home), TMPDIR=str(self.root))
        return subprocess.run([SHELL, str(SCRIPT), *args], env=env, capture_output=True, text=True, timeout=60)

    def csr(self, purpose, *extra, machine=MACHINE):
        result = self.run_script('csr', '--purpose', purpose, '--machine-code', machine, *extra)
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertTrue(result.stdout.startswith('-----BEGIN CERTIFICATE REQUEST-----'))
        self.assertNotIn('PRIVATE KEY', result.stdout + result.stderr)
        return result.stdout

    def install(self, purpose, *extra, chain=None, ca=None, machine=MACHINE):
        return self.run_script('install', '--purpose', purpose, '--machine-code', machine,
                               '--chain', str(chain or self.chain), '--ca', str(ca or self.ca.bundle), *extra)

    def enroll(self, purpose, *csr_extra):
        self.ca.sign(self.csr(purpose, *csr_extra), purpose, self.chain)
        result = self.install(purpose)
        self.assertEqual(0, result.returncode, result.stderr)
        return dict(line.split('=', 1) for line in result.stdout.splitlines() if '=' in line)

    def files(self, purpose):
        if purpose == 'mqtt':
            d = self.home / 'config/runtime/tls'
            return d / f'{MACHINE}-client.key', d / f'{MACHINE}-client.pem', d / 'ca.crt'
        d = self.home / 'config/maintenance-v2'
        return d / 'server.key', d / 'server.crt', None

    def test_csr_keeps_one_pending_key_until_asked_for_a_new_one(self):
        first = self.csr('mqtt')
        key, _, _ = self.files('mqtt')
        pending = Path(str(key) + '.pending')
        self.assertEqual(0o600, stat.S_IMODE(pending.stat().st_mode))
        self.assertEqual(0o700, stat.S_IMODE(pending.parent.stat().st_mode))
        text = subprocess.run(['openssl', 'req', '-in', '-', '-noout', '-subject', '-text'], input=first,
                              capture_output=True, text=True, check=True).stdout
        self.assertIn(f'CN = {MACHINE}', text)
        self.assertRegex(text, 'prime256v1|P-256')
        before = pending.read_bytes()
        self.csr('mqtt')
        self.assertEqual(before, pending.read_bytes(), 'csr again reuses the pending key')
        self.csr('mqtt', '--new-key')
        self.assertNotEqual(before, pending.read_bytes())
        self.assertFalse(key.exists(), 'nothing is installed by csr')

    def test_dry_run_then_install_for_both_purposes(self):
        for purpose in ('mqtt', 'maintenance'):
            with self.subTest(purpose=purpose):
                self.ca.sign(self.csr(purpose), purpose, self.chain)
                dry = self.install(purpose, '--dry-run')
                self.assertEqual(0, dry.returncode, dry.stderr)
                self.assertIn('dryRun=1', dry.stdout)
                self.assertIn('key=pending', dry.stdout)
                key, cert, ca = self.files(purpose)
                self.assertFalse(cert.exists() or key.exists())
                self.assertTrue(Path(str(key) + '.pending').exists())
                done = self.install(purpose)
                self.assertEqual(0, done.returncode, done.stderr)
                self.assertIn('installed=1', done.stdout)
                self.assertFalse(Path(str(key) + '.pending').exists())
                self.assertEqual(0o600, stat.S_IMODE(key.stat().st_mode))
                self.assertEqual(2, cert.read_text().count('BEGIN CERTIFICATE'), 'leaf + device CA')
                if ca:
                    self.assertEqual(self.ca.bundle.read_text(), ca.read_text())
                show = self.run_script('show', '--purpose', purpose, '--machine-code', MACHINE)
                fingerprint = [l for l in done.stdout.splitlines() if l.startswith('fingerprint=')][0]
                self.assertIn(fingerprint, show.stdout)
                self.assertRegex(fingerprint, '^fingerprint=([0-9A-F]{2}:){31}[0-9A-F]{2}$')
                # Re-installing the same certificate uses the current key.
                again = self.install(purpose, '--dry-run')
                self.assertIn('key=current', again.stdout)

    def test_refusals(self):
        self.ca.sign(self.csr('mqtt'), 'mqtt', self.chain)
        other_key = self.root / 'other.key'
        openssl('genpkey', '-algorithm', 'EC', '-pkeyopt', 'ec_paramgen_curve:P-256', '-out', str(other_key), cwd=self.root)
        foreign_csr = subprocess.run(['openssl', 'req', '-new', '-key', str(other_key), '-subj', f'/CN={MACHINE}', '-config', str(self.ca.dir / 'req.cnf')],
                                     capture_output=True, text=True, check=True).stdout
        foreign = self.ca.sign(foreign_csr, 'mqtt', self.root / 'foreign.pem')
        rogue = Ca(self.root / 'rogue', 'Rogue')
        leaf_only = self.root / 'leaf-only.pem'
        leaf_only.write_text(self.chain.read_text().split('-----END CERTIFICATE-----')[0] + '-----END CERTIFICATE-----\n')
        cases = [
            ('a certificate for a key this device does not hold', dict(chain=foreign), 4, "does not belong to this device's key"),
            ('a CA bundle from another root', dict(ca=rogue.bundle), 4, 'does not verify'),
            ('a chain without the device CA', dict(chain=leaf_only), 2, 'must contain the device CA'),
            ('another device', dict(machine='GW_TEST_002'), 4, ''),
        ]
        for name, kwargs, code, message in cases:
            with self.subTest(name):
                result = self.install('mqtt', **kwargs)
                self.assertEqual(code, result.returncode, result.stdout + result.stderr)
                self.assertIn(message, result.stderr)
        # A client certificate is not a maintenance server certificate (same key, other purpose's files).
        self.ca.sign(self.csr('maintenance'), 'mqtt', self.chain)
        self.assertEqual(4, self.install('maintenance').returncode)
        self.assertEqual(2, self.run_script('install', '--purpose', 'mqtt', '--machine-code', MACHINE).returncode)
        self.assertEqual(2, self.run_script('csr', '--purpose', 'mqtt', '--machine-code', 'GW TEST').returncode)
        self.assertEqual(2, self.run_script('csr', '--purpose', 'web', '--machine-code', MACHINE).returncode)
        for purpose in ('mqtt', 'maintenance'):
            key, cert, _ = self.files(purpose)
            self.assertFalse(key.exists() or cert.exists(), 'nothing installed by a refused certificate')

    def test_rotation_and_one_step_rollback(self):
        first = self.enroll('mqtt')
        key, cert, _ = self.files('mqtt')
        first_key = key.read_bytes()
        second = self.enroll('mqtt', '--new-key')
        self.assertEqual(first['fingerprint'], second['replacesFingerprint'])
        self.assertNotEqual(first_key, key.read_bytes())
        back = self.run_script('rollback', '--purpose', 'mqtt', '--machine-code', MACHINE)
        self.assertEqual(0, back.returncode, back.stderr)
        self.assertIn('fingerprint=' + first['fingerprint'], back.stdout)
        self.assertEqual(first_key, key.read_bytes())
        self.assertEqual(0o600, stat.S_IMODE(key.stat().st_mode))
        self.assertEqual(3, self.run_script('rollback', '--purpose', 'mqtt', '--machine-code', MACHINE).returncode)
        # Rolling back a first install removes what it added.
        self.enroll('maintenance')
        self.assertEqual(0, self.run_script('rollback', '--purpose', 'maintenance', '--machine-code', MACHINE).returncode)
        mkey, mcert, _ = self.files('maintenance')
        self.assertFalse(mkey.exists() or mcert.exists())
        self.assertEqual(3, self.run_script('show', '--purpose', 'maintenance', '--machine-code', MACHINE).returncode)


class ProductionInitTlsTests(unittest.TestCase):
    """The TLS part of production-init.sh, cut out of the real script: fixed file names, no certificate work,
    and no start or smoke test until the certificate is installed."""

    @staticmethod
    def section(source, start, end):
        a = source.index(start)
        return source[a:source.index(end, a)]

    def run_init_part(self, home, **env):
        source = (REPO / 'deploy/production-init.sh').read_text()
        functions = ''.join(self.section(source, name + '() {', '\n}\n') + '\n}\n' for name in ('truthy', 'broker_implies_tls'))
        code = (functions
                + self.section(source, '# The machine code is the certificate CN', 'esac\n') + 'esac\n'
                + self.section(source, 'tls_requested=0\n', 'export SOURCE_ROOT=')
                + self.section(source, '# Without its certificate the device is not finished', 'if truthy "$INIT_START_SERVICES"; then')
                + 'printf "%s|%s|%s|%s|%s\\n" "$tls_requested" "${INIT_MQTT_CERT_FILE:-}" "${INIT_MQTT_KEY_FILE:-}" "$INIT_START_SERVICES" "$INIT_RUN_SMOKE"\n')
        base = dict(PATH=os.environ['PATH'], GATEWAY_HOME=str(home), INIT_START_SERVICES='1', INIT_RUN_SMOKE='1')
        base.update(env)
        return subprocess.run([SHELL, '-c', 'set -eu\n' + code], env=base, capture_output=True, text=True, timeout=30)

    def test_tls_uses_the_enroll_script_files_and_waits_for_the_certificate(self):
        with tempfile.TemporaryDirectory(prefix='gz-init-tls-') as temp:
            home = Path(temp)
            tls = home / 'config/runtime/tls'
            expected_cert, expected_key = tls / f'{MACHINE}-client.pem', tls / f'{MACHINE}-client.key'
            result = self.run_init_part(home, INIT_MACHINE_CODE=MACHINE, INIT_MQTT_BROKER='tls://broker.invalid:8883')
            self.assertEqual(0, result.returncode, result.stderr)
            self.assertEqual(f'1|{expected_cert}|{expected_key}|0|0', result.stdout.strip().splitlines()[-1])
            self.assertIn('certificate is not installed yet', result.stdout)
            # Site-specific paths are not accepted any more: the files are always the enroll script's.
            result = self.run_init_part(home, INIT_MACHINE_CODE=MACHINE, INIT_MQTT_TLS_ENABLED='true',
                                        INIT_MQTT_CERT_FILE='/elsewhere/client.pem', INIT_MQTT_KEY_FILE='/elsewhere/client.key')
            self.assertEqual(f'1|{expected_cert}|{expected_key}|0|0', result.stdout.strip().splitlines()[-1])
            # Once installed, init starts services and runs the smoke test as asked.
            tls.mkdir(parents=True)
            expected_cert.write_text('x')
            expected_key.write_text('x')
            result = self.run_init_part(home, INIT_MACHINE_CODE=MACHINE, INIT_MQTT_BROKER='tls://broker.invalid:8883')
            self.assertEqual(f'1|{expected_cert}|{expected_key}|1|1', result.stdout.strip())
            # Without TLS nothing changes.
            result = self.run_init_part(home, INIT_MACHINE_CODE=MACHINE, INIT_MQTT_BROKER='tcp://broker.invalid:1883')
            self.assertEqual('0|||1|1', result.stdout.strip())

    def test_machine_code_rule_is_the_same_as_the_enroll_script(self):
        with tempfile.TemporaryDirectory(prefix='gz-init-tls-') as temp:
            for code, ok in (('GW_PROD-001.a', True), ('GW PROD', False), ('GW/001', False), ('', False)):
                with self.subTest(code=code):
                    result = self.run_init_part(Path(temp), INIT_MACHINE_CODE=code, INIT_MQTT_BROKER='tcp://b:1883')
                    self.assertEqual(0 if ok else 2, result.returncode, result.stderr)


if __name__ == '__main__':
    unittest.main(verbosity=2)
