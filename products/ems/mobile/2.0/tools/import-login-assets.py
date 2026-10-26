"""Import observed Lanhu slices; the login form is rendered with real Qt controls."""
import argparse
import hashlib
import json
from pathlib import Path
from PIL import Image

parser = argparse.ArgumentParser()
parser.add_argument('--downloads', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
records = []
for digest, name, size in [
    ('6629f62d46fcee714e2f9557a3f625b0', 'login-background.png', (1440, 810)),
    ('402fdb34e98439dac6ab40f157494719', 'login-user.png', (32, 32)),
    ('80a83b528a35343db3784990e926607a', 'login-password.png', (32, 32)),
    ('a9ad741af925d0d879182ae24fb9934f', 'login-logo.png', (458, 92)),
]:
    source = args.downloads / f'FigmaSlicePNG{digest}.png'
    with Image.open(source) as image:
        source_size = image.size
        image.resize(size, Image.Resampling.LANCZOS).save(args.output / name)
    records.append(dict(file=name, source=f'https://lanhu-oss-2537-2.lanhuapp.com/FigmaSlicePNG{digest}.png',
                        sourceSize=source_size, sourceSha256=hashlib.sha256(source.read_bytes()).hexdigest()))
(args.output / 'login-provenance.json').write_text(json.dumps(records, indent=2), encoding='utf-8')
