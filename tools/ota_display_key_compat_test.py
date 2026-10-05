"""Extract production display correlation code; local C++14/17, not ARM evidence."""
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
parser.add_argument('--include', type=Path, required=True)
parser.add_argument('--compiler', type=Path, required=True)
parser.add_argument('--evidence', type=Path, required=True)
args = parser.parse_args()
args.evidence.mkdir(exist_ok=False)
source_bytes = args.source.read_bytes()
source = source_bytes.decode('utf-8').replace('\r\n', '\n')
start = source.index('using OtaDisplayKey = ')
end = source.index('\nvoid appendOtaStatus(', start)
key_code = source[start:end]
start = source.index('    struct DisplayLine {', source.index('std::string otaStatusJson('))
end = source.index('\n    std::vector<edge_gateway::OtaStatus> finals;', start)
group_code = source[start:end]
start = source.index('        lines.erase(std::remove_if(lines.begin(), lines.end(),', end)
end = source.index('        }), lines.end());', start) + len('        }), lines.end());')
terminal_code = source[start:end]
legacy = 'std::optional<OtaDisplayKey>' in key_code
includes = '''#include <algorithm>
#include <iterator>
#include <list>
#include <map>
#include <sstream>
#include <set>
#include <string>
#include <vector>
#include <stdexcept>
#include "edge_gateway/json_value.hpp"
'''
if legacy:
    includes += '#include <optional>\n'
unit = includes + '''
std::string trim(const std::string& value) {
    const auto first = value.find_first_not_of(" \\t\\r\\n");
    if (first == std::string::npos) return {};
    return value.substr(first, value.find_last_not_of(" \\t\\r\\n") - first + 1);
}
''' + key_code + '''
std::vector<std::string> group(const std::string& text, bool& truncated) {
    std::istringstream statusInput(text);
    std::string line;
    std::vector<std::string> lines;
''' + group_code + '\n    return lines;\n}\n'
unit += 'void filter(std::vector<std::string>& lines, const std::set<OtaDisplayKey>& terminalIds) {\n' + terminal_code + '\n}\n'
if legacy:
    unit += 'bool valid(const std::string& text) { return bool(otaDisplayKey(text)); }\n'
else:
    unit += 'bool valid(const std::string& text) { OtaDisplayKey key; return otaDisplayKey(text, key); }\n'
unit += r'''
void require(bool pass) { if (!pass) throw std::runtime_error("display correlation mismatch"); }
int main() {
    require(valid(R"({"jobId":"A","machineCode":"M1"})"));
    for (const auto& value : {"[]", "null", "bad", R"({"jobId":"A"})",
        R"({"jobId":"A","machineCode":1})", R"({"jobId":"","machineCode":"M1"})",
        R"({"jobId":"A","jobId":"B","machineCode":"M1"})"}) require(!valid(value));
    require(!valid("{\"jobId\":\"" + std::string(129, 'A') + "\",\"machineCode\":\"M1\"}"));
    require(!valid("{\"jobId\":\"A\",\"machineCode\":\"" + std::string(129, 'M') + "\"}"));
    const std::string old = R"({"jobId":"A","machineCode":"M1","stage":"download"})";
    const std::string fresh = R"({"jobId":"A","machineCode":"M1","occurrenceId":"event-2","stage":"apply"})";
    const std::string otherMachine = R"({"jobId":"A","machineCode":"M2","occurrenceId":"event-3"})";
    const std::string otherJob = R"({"jobId":"B","machineCode":"M1","occurrenceId":"event-4"})";
    bool truncated = false;
    auto values = group(old + "\n" + otherMachine + "\n" + otherJob + "\n" + fresh + "\n", truncated);
    require(values == std::vector<std::string>{otherMachine, otherJob, fresh} && !truncated);
    values.push_back("{\"jobId\":\"A\"}");
    filter(values, {{"A", "M1"}});
    require(values == std::vector<std::string>{otherMachine, otherJob, "{\"jobId\":\"A\"}"});
    const std::string again = R"({"jobId":"A","machineCode":"M1","occurrenceId":"event-5"})";
    values = group(fresh + "\n" + again + "\n", truncated);
    require(values == std::vector<std::string>{again});
    require(group("{\"jobId\":\"A\"}\n{\"jobId\":\"B\"}\n", truncated).size() == 2);
    std::string many = old + "\n";
    for (int i = 0; i < 101; ++i)
        many += "{\"jobId\":\"J" + std::to_string(i) + "\",\"machineCode\":\"M1\"}\n";
    many += fresh + "\n";
    values = group(many, truncated);
    require(truncated && values.size() == 100 && values.back() == fresh);
    return 0;
}
'''


def record(path):
    data = path.read_bytes()
    return dict(sizeBytes=len(data), sha256=hashlib.sha256(data).hexdigest())


def save(name, value):
    with (args.evidence / name).open('x', encoding='ascii') as out:
        json.dump(value, out, sort_keys=True, indent=2)
        out.write('\n')


path = args.evidence / 'production-display-extracted.cpp'
with path.open('x', encoding='ascii', newline='\n') as out:
    out.write(unit)
env = {name: os.environ[name] for name in ('SystemRoot', 'WINDIR', 'TEMP', 'TMP', 'LOCALAPPDATA')}
env['PATH'] = str(args.compiler.parent) + ';' + env['SystemRoot'] + '\\System32'
save('inputs.json', dict(source=str(args.source), sourceRecord=record(args.source),
    parserHeader=record(args.include / 'edge_gateway/json_value.hpp'), compiler=record(args.compiler),
    generatedUnit=record(path), keyCode=key_code, groupCode=group_code, terminalCode=terminal_code,
    boundary='Actual production key/group code and JSON parser; local MinGW, not GCC6/ARM/HTTP/crypto/terminal evidence'))


def run(name, argv):
    result = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=20, env=env)
    for stream in ('stdout', 'stderr'):
        with (args.evidence / (name + '.' + stream + '.raw')).open('xb') as out:
            out.write(getattr(result, stream))
    save(name + '.result.json', dict(argv=[str(x) for x in argv], actualNativeExit=result.returncode))
    return result


class DisplayCompatibility(unittest.TestCase):
    def check_standard(self, standard):
        binary = args.evidence / (standard + '.exe')
        result = run(standard + '-compile', [str(args.compiler), '-std=' + standard,
            '-static-libgcc', '-static-libstdc++', '-I', str(args.include), str(path), '-o', str(binary)])
        self.assertEqual(result.returncode, 0, result.stderr.decode('utf-8', errors='replace'))
        result = run(standard + '-execute', [str(binary)])
        self.assertEqual(result.returncode, 0, result.stderr.decode('utf-8', errors='replace'))

    def test_without_cxx17_optional(self):
        self.check_standard('c++14')

    def test_cxx17_display_semantics(self):
        self.check_standard('c++17')


result = unittest.main(argv=[sys.argv[0]], verbosity=2, exit=False).result
save('summary.json', dict(tests=result.testsRun, failures=len(result.failures), errors=len(result.errors),
    skips=len(result.skipped), sourceUnchanged=record(args.source)['sha256'] == hashlib.sha256(source_bytes).hexdigest(),
    actualArmCompiler=False, productionApproval='NO'))
sys.exit(0 if result.wasSuccessful() else 1)
