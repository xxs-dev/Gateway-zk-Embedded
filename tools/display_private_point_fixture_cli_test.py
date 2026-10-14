"""Exact fixture CLI negatives under caller limits; never creates a store or lifts limits."""
import json
import os
from pathlib import Path
import resource
import subprocess
import sys


def main():
    if len(sys.argv) != 3 or os.geteuid() == 0:
        raise ValueError('NONROOT_HELPER_AND_FRESH_CAPTURE_REQUIRED')
    helper = Path(sys.argv[1]).resolve(strict=True)
    out = Path(sys.argv[2])
    out.mkdir(mode=0o700)
    assert resource.getrlimit(resource.RLIMIT_AS)[0] <= 512*1024*1024
    assert resource.getrlimit(resource.RLIMIT_FSIZE) == (8*1024*1024, 8*1024*1024)
    names = [Path('/dev/shm/d24_display_20261011_01_'+suffix) for suffix in ('live', 'test')]
    assert all(not os.path.lexists(p) for p in names), 'scope object already present'
    base = ['--run', '--name', 'd24_display_20261011_01_test']
    vectors = [
        ('wrong-name', ['--run', '--name', 'gateway_point_store'], 'EXACT_SCOPE_PRIVATE_NAME_REQUIRED'),
        ('wrong-index', base+['--temperature-index', '880003'], 'FIXED_INDEX_REQUIRED'),
        ('negative-index', base+['--counter-index', '-1'], 'UNSIGNED_INTEGER_REQUIRED'),
        ('nan', base+['--a-temperature', 'nan'], 'FINITE_CANONICAL_VALUE_REQUIRED'),
        ('inf', base+['--a-temperature', 'inf'], 'FINITE_CANONICAL_VALUE_REQUIRED'),
        ('minus-inf', base+['--a-temperature', '-inf'], 'FINITE_CANONICAL_VALUE_REQUIRED'),
        ('overflow-float', base+['--a-temperature', '1e400'], 'FINITE_CANONICAL_VALUE_REQUIRED'),
        ('trailing-float', base+['--a-temperature', '12.5junk'], 'FINITE_CANONICAL_VALUE_REQUIRED'),
        ('different-finite', base+['--b-temperature', '37.5'], 'FIXED_VALUE_REQUIRED'),
        ('zero-duration', base+['--phase-seconds', '0'], 'BOUNDED_PHASE_REQUIRED'),
        ('over-duration', base+['--phase-seconds', '41'], 'BOUNDED_PHASE_REQUIRED'),
        ('duplicate-option', base+['--name', 'd24_display_20261011_01_test'], 'BOUNDED_UNIQUE_OPTIONS_REQUIRED'),
        ('unknown-option', base+['--other', '1'], 'UNKNOWN_OPTION'),
        ('insufficient-fsize', base, 'FSIZE_BELOW_REAL_V11_LAYOUT'),
    ]
    results = []
    for name, argv, reason in vectors:
        case = out/name
        case.mkdir()
        command = ['/usr/bin/timeout', '--signal=TERM', '--kill-after=1s', '5s', str(helper), *argv]
        (case/'command.json').write_text(json.dumps(command)+'\n', encoding='ascii')
        with (case/'stdout.raw').open('xb') as stdout, (case/'stderr.raw').open('xb') as stderr:
            process = subprocess.run(command, stdin=subprocess.DEVNULL, stdout=stdout, stderr=stderr,
                                     timeout=8, check=False)
        raw = (case/'stderr.raw').read_bytes()
        assert process.returncode == 2, (name, process.returncode)
        assert raw == ('PRIVATE_FIXTURE_REJECTED '+reason+'\n').encode('ascii'), (name, raw)
        assert (case/'stdout.raw').stat().st_size == 0
        assert all(not os.path.lexists(p) for p in names), (name, 'unexpected scope allocation')
        result = dict(name=name, actualExit=process.returncode, reason=reason, scopeObjectsAbsent=True)
        with (case/'result.json').open('x', encoding='ascii') as stream:
            json.dump(result, stream, sort_keys=True)
            stream.write('\n')
        results.append(result)
    summary = dict(status='CLI_DENIALS_PASS_NOT_RESOLVED_POSITIVE', cases=results, caseCount=len(results),
                   uid=os.geteuid(), fsizeBytes=resource.getrlimit(resource.RLIMIT_FSIZE)[0],
                   positiveApiProducerAndReplacementExecuted=False)
    with (out/'result.json').open('x', encoding='ascii') as stream:
        json.dump(summary, stream, sort_keys=True, indent=2)
        stream.write('\n')
    print(json.dumps(summary, sort_keys=True))


if __name__ == '__main__':
    main()
