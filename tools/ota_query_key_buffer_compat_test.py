"""Narrow source-loop compatibility test; no key/crypto/system/remote execution."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import unittest

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source', type=Path, required=True)
parser.add_argument('--compiler', type=Path, required=True)
parser.add_argument('--evidence', type=Path, required=True)
ARGS = parser.parse_args()
ARGS.evidence.mkdir(exist_ok=False)
SOURCE = ARGS.source.read_bytes()
text = SOURCE.decode('utf-8').replace('\r\n', '\n')
start = text.index('    std::size_t received = 0;', text.index('std::string readPinnedKey('))
end = text.index('    char extra;', start)
LOOP = text[start:end]
if 'while (received < bytes.size())' not in LOOP or LOOP.count('read(fd,') != 1:
    raise ValueError('Expected original guarded read loop absent')


def save(name, value):
    with (ARGS.evidence / name).open('x', encoding='ascii') as out:
        json.dump(value, out, sort_keys=True, indent=2)
        out.write('\n')


def record(path):
    data = path.read_bytes()
    return dict(sha256=hashlib.sha256(data).hexdigest(), sizeBytes=len(data))


env = {name: os.environ[name] for name in ('SystemRoot', 'WINDIR', 'TEMP', 'TMP', 'LOCALAPPDATA')}
env['PATH'] = str(ARGS.compiler.parent) + ';' + env['SystemRoot'] + '\\System32'
save('inputs.json', dict(source=str(ARGS.source), sourceRecord=record(ARGS.source),
     compiler=str(ARGS.compiler), compilerRecord=record(ARGS.compiler),
     loopText=LOOP, boundary='Actual guarded source-loop bytes; declared POSIX read type and synthetic partial reader. Not GCC6/ARM/full key policy'))


def run(name, argv):
    result = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=20,
                            env=env, cwd=str(ARGS.evidence))
    for stream in ('stdout', 'stderr'):
        with (ARGS.evidence / (name + '.' + stream + '.raw')).open('xb') as out:
            out.write(getattr(result, stream))
    save(name + '.result.json', dict(argv=[str(x) for x in argv], actualNativeExit=result.returncode,
                                   sourceRecord=record(ARGS.source), remoteActions=0))
    return result


HEAD = '#include <string>\n#include <stdexcept>\n#include <cstddef>\n'
DECLARED = HEAD + 'std::ptrdiff_t read(int, void*, std::size_t);\nint close(int);\n'
TYPE_UNIT = DECLARED + 'void contract(int fd, std::string& bytes) {\n' + LOOP + '}\n'
RUNTIME_HEAD = HEAD + '''#include <algorithm>
#include <cstring>
#include <iostream>
static const char input[] = "fixture-bytes";
static std::size_t position = 0;
static bool shortRead = false;
static int closed = 0;
std::ptrdiff_t read(int, void* target, std::size_t count) {
    if (shortRead && position >= 3) return 0;
    const auto size = std::min<std::size_t>(3, std::min(count, sizeof(input) - 1 - position));
    std::memcpy(target, input + position, size);
    position += size;
    return static_cast<std::ptrdiff_t>(size);
}
int close(int) { ++closed; return 0; }
void contract(int fd, std::string& bytes) {
'''
RUNTIME_TAIL = '''}
int main() {
    std::string bytes(sizeof(input) - 1, '?');
    contract(1, bytes);
    if (bytes != input || position != sizeof(input) - 1 || closed != 0) return 1;
    position = 0;
    bytes.clear();
    contract(1, bytes);
    if (position != 0 || closed != 0) return 2;
    bytes.resize(sizeof(input) - 1);
    shortRead = true;
    try { contract(1, bytes); return 3; }
    catch (const std::runtime_error& error) {
        if (std::string(error.what()) != "short OTA query key read" || closed != 1) return 4;
    }
    std::cout << "PARTIAL_BYTES_MATCH EMPTY_LOOP_NO_ACCESS SHORT_READ_REJECTED\\n";
    return 0;
}
'''


class WritableLoop(unittest.TestCase):
    def test_actual_loop_typechecks_const_data_legacy_and_cxx17(self):
        path = ARGS.evidence / 'source-loop-type.cpp'
        path.write_text(TYPE_UNIT, encoding='ascii')
        for standard in ('c++14', 'c++17'):
            with self.subTest(standard=standard):
                result = run('type-' + standard, [str(ARGS.compiler), '-std=' + standard, '-fsyntax-only', str(path)])
                self.assertEqual(result.returncode, 0, result.stderr.decode('utf-8', errors='replace'))

    def test_actual_loop_partial_empty_and_short_read(self):
        path = ARGS.evidence / 'source-loop-runtime.cpp'
        path.write_text(RUNTIME_HEAD + LOOP + RUNTIME_TAIL, encoding='ascii')
        binary = ARGS.evidence / 'source-loop-runtime.exe'
        result = run('runtime-compile', [str(ARGS.compiler), '-std=c++14', '-static-libgcc', '-static-libstdc++',
                                      str(path), '-o', str(binary)])
        self.assertEqual(result.returncode, 0, result.stderr.decode('utf-8', errors='replace'))
        result = run('runtime-execute', [str(binary)])
        self.assertEqual(result.returncode, 0, result.stderr.decode('utf-8', errors='replace'))
        self.assertIn(b'PARTIAL_BYTES_MATCH EMPTY_LOOP_NO_ACCESS SHORT_READ_REJECTED', result.stdout)


if __name__ == '__main__':
    result = unittest.main(argv=[sys.argv[0]], verbosity=2, exit=False).result
    save('summary.json', dict(tests=result.testsRun, failures=len(result.failures), errors=len(result.errors),
                             skips=len(result.skipped), sourceUnchanged=record(ARGS.source)['sha256'] == hashlib.sha256(SOURCE).hexdigest(),
                             actualArmCompiler=False, actualKeysOrCrypto=False, productionApproval='NO'))
    sys.exit(0 if result.wasSuccessful() else 1)
