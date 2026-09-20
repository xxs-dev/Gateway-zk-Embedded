#!/usr/bin/env python3
"""Build checksum-only test candidates from pinned, already-built programs."""
import argparse
import gzip
import hashlib
import io
import json
from pathlib import Path
import subprocess
import tarfile

REPO = Path(__file__).resolve().parents[1]
BINARY_COMMIT = '5d40272e61e708e4694b4bdc6f15e2886efcd73e'
ARCHIVE_SHA = 'd7c6646370c884496b072a226de12889dea4931742cf867ffeb9cdb33c1bff09'
MANIFEST_SHA = 'bdb874460cf7cfbb6c2d77514b5f21de515893ad01f4185fe778da4a499ad404'
BASE_APPLY_SHA = '3c6018fc302197ed171523f69704f5c0be110c5614d2c0dc29d3562951bfd6d7'
ROLLBACK_SHA = '311019b0e8ae0d1334e4fb5c1a27169aaa2ec186c3c76b322e5ca23edbdd9bc1'
MQTT_SHA = 'f6a83a45711407c58f67b5fa787adf2de3a42f87e26b69f5e9ab92386299a07b'
PREFIX = '/opt/modbus-gateway/bin/'
SUCCESS = 'test-20260920-ota-manifest-01'
FAILURE = 'test-20260920-ota-rollback-01'


def digest(data):
    return hashlib.sha256(data).hexdigest()


def encoded(value):
    return (json.dumps(value, indent=2, sort_keys=True) + '\n').encode()


def checked(path, expected):
    data = path.read_bytes()
    if digest(data) != expected:
        raise ValueError('source hash mismatch: ' + str(path))
    return data


def pack(output, version, source, payloads, restarts, fail_second=False):
    allowed = {'ota-apply.sh', 'ota-rollback.sh'} if fail_second else {'ota-apply.sh', 'MqttDriver'}
    if set(payloads) != allowed:
        raise ValueError('unexpected target allowlist')
    files = [{'path': 'bin/' + name, 'target': PREFIX + name,
              'sha256': '0' * 64 if fail_second and name == 'ota-rollback.sh' else digest(data)}
             for name, data in payloads.items()]
    manifest = {'packageType': 'programs', 'testOnly': True, 'version': version,
                'sourceCommit': source, 'binarySourceCommit': BINARY_COMMIT,
                'files': files, 'restart': {'services': restarts}}
    members = {'manifest.json': encoded(manifest)}
    members.update({'bin/' + name: data for name, data in payloads.items()})
    artifact = output / (version + '.tar.gz')
    with artifact.open('xb') as raw:
        with gzip.GzipFile(filename='', fileobj=raw, mode='wb', mtime=0) as gz:
            with tarfile.open(fileobj=gz, mode='w', format=tarfile.USTAR_FORMAT) as archive:
                for name, data in members.items():
                    info = tarfile.TarInfo(name)
                    info.size = len(data)
                    info.mode = 0o644 if name == 'manifest.json' else 0o755
                    info.uid = info.gid = 0
                    info.mtime = 0
                    archive.addfile(info, io.BytesIO(data))
    (output / (version + '.manifest.json')).write_bytes(encoded(manifest))
    return {'file': artifact.name, 'version': version, 'sizeBytes': artifact.stat().st_size,
            'sha256': digest(artifact.read_bytes()), 'uncompressedFileBytes': sum(map(len, members.values())),
            'manifest': manifest, 'payloadSha256': {name: digest(data) for name, data in payloads.items()},
            'intentionalInvalidInternalSha256': 'bin/ota-rollback.sh' if fail_second else None,
            'actualContentChanges': [PREFIX + 'ota-apply.sh'],
            'sameByteReinstall': [] if fail_second else [PREFIX + 'MqttDriver'],
            'mustNotBeCopied': [PREFIX + 'ota-rollback.sh'] if fail_second else []}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--programs', type=Path, required=True)
    parser.add_argument('--program-manifest', type=Path, required=True)
    parser.add_argument('--baseline-apply', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    source = subprocess.check_output(['git', '-C', str(REPO), 'rev-parse', 'HEAD'], text=True).strip()
    dirty = subprocess.check_output(['git', '-C', str(REPO), 'status', '--porcelain'], text=True)
    if dirty:
        parser.error('commit candidate source first; dirty builds are prohibited')
    checked(args.baseline_apply, BASE_APPLY_SHA)
    archive_data = checked(args.programs, ARCHIVE_SHA)
    build_manifest = json.loads(checked(args.program_manifest, MANIFEST_SHA))
    if build_manifest['sourceCommit'] != BINARY_COMMIT:
        parser.error('unexpected binary provenance')
    with tarfile.open(fileobj=io.BytesIO(archive_data), mode='r:gz') as archive:
        mqtt = archive.extractfile('bin/MqttDriver').read()
    if digest(mqtt) != MQTT_SHA:
        parser.error('MqttDriver component hash mismatch')
    component = next(item for item in build_manifest['components'] if item['binary'] == 'MqttDriver')
    if component['sha256'] != MQTT_SHA or component['sizeBytes'] != len(mqtt):
        parser.error('MqttDriver manifest mismatch')
    apply = (REPO / 'deploy/ota-apply.sh').read_bytes()
    rollback = checked(REPO / 'deploy/ota-rollback.sh', ROLLBACK_SHA)
    args.output.mkdir(parents=True, exist_ok=False)
    success = pack(args.output, SUCCESS, source, {'ota-apply.sh': apply, 'MqttDriver': mqtt},
                   ['mqtt-driver@mqtt-service.service'])
    failure = pack(args.output, FAILURE, source,
                   {'ota-apply.sh': apply + b'\n# Test-only partial-apply rollback receipt.\n',
                    'ota-rollback.sh': rollback}, [], fail_second=True)
    provenance = {'schemaVersion': 1, 'testOnly': True, 'deploymentAuthorized': False,
                  'sourceCommit': source, 'sourceDirty': False, 'binarySourceCommit': BINARY_COMMIT,
                  'programArchiveSha256': ARCHIVE_SHA, 'programManifestSha256': MANIFEST_SHA,
                  'baseApplySha256': BASE_APPLY_SHA, 'fixedApplySha256': digest(apply),
                  'rollbackSha256': ROLLBACK_SHA, 'signature': None,
                  'integrityMode': 'SHA256 checksum only; not a digital signature',
                  'success': success, 'partialFailure': failure,
                  'constraints': ['Coordinator must review exact hashes before device execution',
                                  'Success must precede the partial-failure fixture',
                                  'Preserve 151 points, identity, TLS, network and full configuration',
                                  'Only MQTT service restart; no reboot, collector or monitor restart',
                                  'Capture current device write-set backups and confirm space first',
                                  'Restore temporary OTA-enabled configuration exactly after test']}
    (args.output / 'provenance.json').write_bytes(encoded(provenance))
    print(encoded(provenance).decode())


if __name__ == '__main__':
    main()
