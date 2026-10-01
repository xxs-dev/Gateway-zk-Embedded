"""Narrow local source checks/minimized old helper timing; never device/WSL.

The optional RED compiles exact historical helper bodies on the local compiler.
It is not full SystemMonitor, POSIX wait-status or AArch64 acceptance.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[1]
BASELINE = '18b600b0aa0d4a15f602fb830b3078dd5102befc'


class SourceContract(unittest.TestCase):
    def test_diag_entry_is_nonblocking_and_loop_polls_before_safety(self):
        source = (REPO / 'src/system_monitor_service.cpp').read_text(encoding='utf-8')
        handler = source.split('void SystemMonitorService::handleDiagRequest(', 1)[1].split(
            'std::string SystemMonitorService::diagnosticReply(', 1)[0]
        self.assertNotIn('runShellCommand(', handler)
        self.assertIn('diagnosticProcess_.start(', handler)
        loop = source.split('void SystemMonitorService::runOnce(', 1)[1].split(
            'void SystemMonitorService::publishCellularPoints(', 1)[0]
        self.assertLess(loop.index('pollDiagnostics(nowMs)'), loop.index('scadaSafetyMonitor_->runOnce'))

    def test_inflight_has_no_false_terminal_reply_and_conflict_is_strict(self):
        source = (REPO / 'src/system_monitor_service.cpp').read_text(encoding='utf-8')
        duplicate = source.split('if (existing != diagnosticRequests_.end()) {', 1)[1].split(
            'if (!isCommandAllowed(command))', 1)[0]
        duplicate = '\n'.join(line.split('//', 1)[0] for line in duplicate.splitlines())
        self.assertIn('saved.command != command || saved.arg != arg || saved.operatorName != operatorName', duplicate)
        self.assertIn('cmdId_conflict', duplicate)
        self.assertNotIn('"in_progress"', duplicate)
        self.assertIn('publishReply(saved.reply)', duplicate)
        self.assertIn('kMaxDiagnosticReplies = 16', source)
        self.assertIn('std::chrono::minutes(5)', source)

    def test_no_worker_thread_or_parent_global_child_reaping(self):
        source = (REPO / 'src/diagnostic_process.cpp').read_text(encoding='utf-8')
        parent = source.split('DiagnosticProcess::~DiagnosticProcess()', 1)[1]
        self.assertNotIn('waitpid(-1', parent)
        self.assertNotIn('std::thread(', source)
        self.assertNotIn('popen(', source)
        self.assertIn('PR_SET_CHILD_SUBREAPER', source)
        self.assertIn('MSG_DONTWAIT | MSG_NOSIGNAL', source)


def reproduce():
    raw = subprocess.check_output(['git', 'show', BASELINE + ':src/system_monitor_service.cpp'], cwd=REPO).decode()
    truncate = raw[raw.index('std::string truncateText('):raw.index('\nbool startsWith(', raw.index('std::string truncateText('))]
    helper = raw[raw.index('std::string runShellCommand('):raw.index('\nint hexDigit(', raw.index('std::string runShellCommand('))]
    code = ('#include <algorithm>\n#include <chrono>\n#include <cstdio>\n#include <cstdlib>\n'
            '#include <cstring>\n#include <iostream>\n#include <string>\n' + truncate + helper + '''
int main() {
 const auto start=std::chrono::steady_clock::now(); int exit=0;
 const auto output=runShellCommand(std::getenv("PRIVATE_DIAG_COMMAND"), &exit, 256);
 const auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(
  std::chrono::steady_clock::now()-start).count();
 std::cout << "old helper elapsedMs=" << elapsed << " commandExit=" << exit
           << " outputBytes=" << output.size() << "\\\\n";
 if (exit != 0 || output.empty()) return 2;
 return elapsed < 100 ? 0 : 1;
}
''')
    with tempfile.TemporaryDirectory(prefix='diag-old-helper-') as directory:
        root = Path(directory); source = root / 'probe.cpp'; executable = root / ('probe.exe' if os.name == 'nt' else 'probe')
        source.write_text(code, encoding='utf-8')
        built = subprocess.run(['g++', '-std=c++17', str(source), '-o', str(executable)],
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=30)
        print(built.stdout, end='')
        if built.returncode: return built.returncode
        env = dict(os.environ, PRIVATE_DIAG_COMMAND=subprocess.list2cmdline([
            sys.executable, '-c', 'import time; time.sleep(0.4); print(1)']))
        result = subprocess.run([str(executable)], env=env, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True, timeout=5)
        print(json.dumps({'baseline': BASELINE, 'scope': 'minimized exact helper, local only',
                          'stdout': result.stdout, 'exitCode': result.returncode}))
        return result.returncode


def portable():
    with tempfile.TemporaryDirectory(prefix='diag-portable-') as directory:
        executable = Path(directory) / 'diagnostic-process-test.exe'
        command = ['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                   '-DEDGE_DIAGNOSTIC_PROCESS_TEST_HOOKS', '-I', str(REPO / 'include'),
                   str(REPO / 'src/diagnostic_process.cpp'), str(REPO / 'tools/diagnostic_process_test.cpp'),
                   '-o', str(executable)]
        built = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=30)
        print(built.stdout, end='')
        if built.returncode: return built.returncode
        result = subprocess.run([str(executable)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=5)
        print(result.stdout, end='')
        print(json.dumps({'compilerExitCode': built.returncode, 'testExitCode': result.returncode,
                          'scope': 'local native compiler only; not AArch64 product build'}))
        return result.returncode


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--reproduce-baseline', action='store_true')
    parser.add_argument('--compile-portable', action='store_true')
    args, remainder = parser.parse_known_args()
    if args.reproduce_baseline: sys.exit(reproduce())
    if args.compile_portable: sys.exit(portable())
    unittest.main(argv=[sys.argv[0]] + remainder, verbosity=2)
