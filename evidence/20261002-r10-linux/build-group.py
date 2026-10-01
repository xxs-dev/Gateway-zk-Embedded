"""Compiler PID1 admission; kernel closes all descendants on this PID1's exit."""
import json
import os
import sys

assert os.getpid() == 1
proof = dict(pidNamespace=os.readlink('/proc/self/ns/pid'))
print(json.dumps(proof), flush=True)
assert sys.stdin.readline() == 'GO\n'
argv = json.loads(sys.argv[1])
assert isinstance(argv, list) and argv and all(isinstance(item, str) for item in argv)
os.execvpe(argv[0], argv, os.environ)
