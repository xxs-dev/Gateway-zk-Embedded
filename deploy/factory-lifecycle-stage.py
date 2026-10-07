#!/usr/bin/env python3
"""Stage one paired factory lifecycle source; never enable CORE test admission."""
import argparse
import ast
import hashlib
import io
import os
from pathlib import Path
import stat
import tempfile
import tokenize

NAMES = ('offline-runtime-upgrade.py', 'runtime-upgrade-guard.py', 'ota-health-result.py',
         'production_role_policy.py')
POLICY = 'CORE_TEST_APPLY_ALLOWED'


def disabled_entry(raw):
    tree = ast.parse(raw)
    bindings = [n for n in ast.walk(tree) if isinstance(n, ast.Name) and n.id == POLICY and isinstance(n.ctx, ast.Store)]
    other_bindings = [n for n in ast.walk(tree) if
                      (isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef, ast.ExceptHandler)) and
                       n.name == POLICY) or (isinstance(n, ast.arg) and n.arg == POLICY) or
                      (isinstance(n, ast.alias) and (n.asname or n.name.split('.')[0]) == POLICY)]
    declarations = [n for n in tree.body if isinstance(n, ast.Assign) and len(n.targets) == 1 and
                    isinstance(n.targets[0], ast.Name) and n.targets[0].id == POLICY]
    value = declarations[0].value if len(declarations) == 1 else None
    literal_bool = (isinstance(value, getattr(ast, 'Constant', ())) or
                    isinstance(value, getattr(ast, 'NameConstant', ()))) and type(getattr(value, 'value', None)) is bool
    if len(bindings) != 1 or other_bindings or len(declarations) != 1 or not literal_bool:
        raise ValueError('factory core policy assignment must be one literal bool with no other binding')
    # Python 3.6 has NameConstant and no AST end positions. Token columns are
    # characters, whereas AST columns are UTF-8 bytes, even with a coding cookie.
    encoding, _ = tokenize.detect_encoding(io.BytesIO(raw).readline)
    lines = io.StringIO(raw.decode(encoding)).readlines()
    expected = 'True' if value.value else 'False'
    tokens = [token for token in tokenize.tokenize(io.BytesIO(raw).readline)
              if token.type == tokenize.NAME and token.string == expected and token.start[0] == value.lineno
              and len(lines[token.start[0] - 1][:token.start[1]].encode('utf-8')) == value.col_offset]
    if len(tokens) != 1:
        raise ValueError('factory core policy assignment literal token mismatch')
    token = tokens[0]
    begin = len((''.join(lines[:token.start[0] - 1]) + lines[token.start[0] - 1][:token.start[1]]).encode(encoding))
    end = len((''.join(lines[:token.end[0] - 1]) + lines[token.end[0] - 1][:token.end[1]]).encode(encoding))
    if raw[begin:end] != expected.encode('ascii'):
        raise ValueError('factory core policy assignment literal byte mismatch')
    staged = raw[:begin] + b'False' + raw[end:]
    compile(staged, '<factory-core-disabled>', 'exec')
    return staged


def stage(source, output):
    source, output = Path(source), Path(output)
    if any(p.is_symlink() for p in (source,) + tuple(source.parents) + (output,) + tuple(output.parents)):
        raise ValueError('factory lifecycle directory must not contain symlinks')
    payload = {}
    for name in NAMES:
        path = source / name
        before = path.lstat()
        if not stat.S_ISREG(before.st_mode) or path.is_symlink() or before.st_size > 1024**2:
            raise ValueError('invalid factory lifecycle source: ' + name)
        with path.open('rb') as stream:
            opened = os.fstat(stream.fileno())
            raw = stream.read(1024**2 + 1)
            # Windows path stat reports creation time as ctime, but fstat may
            # report change time. Keep ctime on the named before/after check.
            identity = lambda s: (s.st_dev, s.st_ino, s.st_size, s.st_mtime_ns)
            after = path.lstat()
            if (len(raw) != before.st_size or identity(before) != identity(opened) or
                    identity(opened) != identity(after) or before.st_ctime_ns != after.st_ctime_ns or
                    (os.name != 'nt' and before.st_ctime_ns != opened.st_ctime_ns)):
                raise ValueError('factory lifecycle source drift: ' + name)
        compile(raw, str(path), 'exec')
        payload[name] = disabled_entry(raw) if name == NAMES[0] else raw
    # All three inputs and policy are validated before the first staged replacement.
    output.mkdir(parents=True, exist_ok=True)
    for name, raw in payload.items():
        destination = output / name
        if destination.is_symlink():
            raise ValueError('symlink factory staging destination')
        fd, temporary = tempfile.mkstemp(prefix='.' + name + '.', dir=str(output))
        try:
            with os.fdopen(fd, 'wb') as stream:
                stream.write(raw); stream.flush(); os.fsync(stream.fileno())
            os.chmod(temporary, 0o755)
            os.replace(temporary, destination)
        finally:
            if os.path.exists(temporary): os.unlink(temporary)
    return {name: hashlib.sha256(raw).hexdigest() for name, raw in payload.items()}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('source'); parser.add_argument('output')
    args = parser.parse_args()
    stage(args.source, args.output)


if __name__ == '__main__':
    main()
