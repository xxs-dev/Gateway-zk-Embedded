"""B2 root binding for the fixed B1 admission and dependency inspector."""
import hashlib
from pathlib import Path

base = Path('/tmp/gw-r10-eb6cc53-23-B2/source/environment-base.py').read_bytes()
assert hashlib.sha256(base).hexdigest() == '25314a64846123cf88aca97283359e6ae2c0cd5176e02a85c8cf3be15ee9140c'
exec(compile(base, 'environment-base.py', 'exec'), globals())
ROOT = '/tmp/gw-r10-eb6cc53-23-B2'
