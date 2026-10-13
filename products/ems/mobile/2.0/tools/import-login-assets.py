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
    ('38d56a6b6e5f21b4cf7e674e78901fc0', 'login-background.png', (1440, 810)),
    ('402fdb34e98439dac6ab40f157494719', 'login-user.png', (32, 32)),
    ('80a83b528a35343db3784990e926607a', 'login-password.png', (32, 32)),
]:
    source = args.downloads / f'FigmaSlicePNG{digest}.png'
    with Image.open(source) as image:
        source_size = image.size
        image.resize(size, Image.Resampling.LANCZOS).save(args.output / name)
    records.append(dict(file=name, source=f'https://lanhu-oss-2537-2.lanhuapp.com/FigmaSlicePNG{digest}.png',
                        sourceSize=source_size, sourceSha256=hashlib.sha256(source.read_bytes()).hexdigest()))
source = args.downloads / 'FigmaCover12b033ed63144c667af53aae621888da.png'
with Image.open(source) as image:
    logo = image.convert('RGBA').crop((68, 80, 300, 129))
    logo.putdata([(r, g, b, 0 if min(r, g, b) > 160 else a) for r, g, b, a in logo.getdata()])
    logo.save(args.output / 'login-logo.png')
records.append(dict(file='login-logo.png', source='https://assets.lanhuapp.com/' + source.name,
                    sourceSha256=hashlib.sha256(source.read_bytes()).hexdigest(), crop=[68, 80, 300, 129]))
(args.output / 'login-provenance.json').write_text(json.dumps(records, indent=2), encoding='utf-8')
