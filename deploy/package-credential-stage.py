#!/usr/bin/env python3
"""Remove authentication values from private package config staging only."""
import json
import os
from pathlib import Path
import stat
import sys

CREDENTIALS = frozenset(('password', 'secretkey', 'accesskey', 'token', 'apikey',
                        'enrollmenttoken', 'privatekey', 'clientsecret'))
LIMIT = 16 * 1024 * 1024


def unique(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError('duplicate field')
        result[key] = value
    return result


def sanitize(value):
    if isinstance(value, dict):
        return {key: '' if key.lower() in CREDENTIALS else sanitize(item)
                for key, item in value.items()}
    if isinstance(value, list):
        return [sanitize(item) for item in value]
    return value


def stage(root):
    root = Path(root)
    if not root.is_dir() or any(p.is_symlink() for p in (root,) + tuple(root.parents)):
        raise ValueError('invalid staging directory')
    changed = 0
    for path in sorted(root.rglob('*.json')):
        if any(p.is_symlink() for p in path.parents if p != root.parent):
            raise ValueError('symlinked staging directory')
        fd = os.open(str(path), os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
        with os.fdopen(fd, 'rb') as stream:
            info = os.fstat(stream.fileno())
            if not stat.S_ISREG(info.st_mode) or info.st_size > LIMIT:
                raise ValueError('invalid staged config')
            raw = stream.read(LIMIT + 1)
        if len(raw) > LIMIT:
            raise ValueError('staged config bound')
        value = json.loads(raw.decode('utf-8'), object_pairs_hook=unique,
                           parse_constant=lambda _: (_ for _ in ()).throw(ValueError('nonfinite number')))
        clean = sanitize(value)
        if clean != value:
            temporary = path.with_name(path.name + '.credential-stage.tmp')
            try:
                with temporary.open('x', encoding='utf-8', newline='\n') as stream:
                    json.dump(clean, stream, ensure_ascii=True, indent=2, allow_nan=False)
                    stream.write('\n')
                os.chmod(str(temporary), stat.S_IMODE(info.st_mode))
                os.replace(str(temporary), str(path))
            finally:
                if temporary.exists():
                    temporary.unlink()
            changed += 1
    return changed


if __name__ == '__main__':
    try:
        if len(sys.argv) != 2:
            raise ValueError('one staging directory required')
        print('PACKAGE_CONFIG_CREDENTIALS_CLEARED files=' + str(stage(sys.argv[1])))
    except Exception as error:
        # Parser errors may carry source text; only a fixed code/category is emitted.
        print('PACKAGE_CONFIG_INVALID category=' + type(error).__name__, file=sys.stderr)
        sys.exit(2)
