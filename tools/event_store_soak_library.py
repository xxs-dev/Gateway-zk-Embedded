"""Validate the fixed private SQLite input without loading it or starting a server."""
import hashlib
from pathlib import Path
import re
import sys


def validate(lab, expected):
    lab = Path(lab).resolve(strict=True)
    if not re.fullmatch('[0-9a-fA-F]{64}', expected):
        raise ValueError('private SQLite requires an explicit SHA256')
    library = lab / 'libsqlite3.so'
    if library.is_symlink() or not library.is_file():
        raise ValueError('private SQLite must be a regular lab/libsqlite3.so, not a symlink')
    entries = []
    for line in (lab / 'SHA256SUMS').read_text(encoding='ascii').splitlines():
        match = re.fullmatch(r'([0-9a-fA-F]{64}) [ *]libsqlite3\.so', line)
        if match:
            entries.append(match[1].lower())
    if entries != [expected.lower()]:
        raise ValueError('SHA256SUMS must contain exactly one matching libsqlite3.so entry')
    digest = hashlib.sha256()
    with library.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    if digest.hexdigest() != expected.lower():
        raise ValueError('private SQLite content SHA256 mismatch')
    return library


if __name__ == '__main__':
    try:
        if len(sys.argv) != 3:
            raise ValueError('usage: event_store_soak_library.py LAB_DIR LIBRARY_SHA256')
        print(validate(sys.argv[1], sys.argv[2]))
    except (OSError, ValueError) as error:
        print(str(error), file=sys.stderr)
        sys.exit(2)
