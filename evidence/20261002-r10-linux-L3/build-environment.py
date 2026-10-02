"""L3 root binding for the pinned L2 environment inspector."""
import hashlib
from pathlib import Path

base = Path('/srv/build/gw-r10-eb6cc53-build-L3/build-environment-base.py').read_bytes()
assert hashlib.sha256(base).hexdigest() == '3d469e73c6f7ba507ecbc93f1e198d288713f4f3e702113b33ac35ce0b4091b4'
exec(compile(base, 'build-environment-base.py', 'exec'), globals())
ROOT = '/srv/build/gw-r10-eb6cc53-build-L3'
