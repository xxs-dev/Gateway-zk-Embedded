"""Import the 2026-10-16 approved slices and localized artwork, never UI text/data."""
import argparse
import hashlib
import json
from pathlib import Path

from PIL import Image, ImageDraw


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--references', type=Path, required=True)
    parser.add_argument('--downloads', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    records = []

    def record(name, source, **details):
        records.append(dict(file=name, source=source.name,
                            sourceSha256=hashlib.sha256(source.read_bytes()).hexdigest(),
                            sha256=hashlib.sha256((args.output / name).read_bytes()).hexdigest(), **details))

    background = args.downloads / 'FigmaSlicePNG6629f62d46fcee714e2f9557a3f625b0.png'
    with Image.open(background) as image:
        if image.size != (3840, 2160):
            raise ValueError('Unexpected login slice size')
        image.resize((1440, 810), Image.Resampling.LANCZOS).save(args.output / 'login-background.png')
    record('login-background.png', background, size=[1440, 810],
           sourceUrl='https://lanhu-oss-2537-2.lanhuapp.com/' + background.name)
    logo = args.downloads / 'FigmaSlicePNGa9ad741af925d0d879182ae24fb9934f.png'
    with Image.open(logo) as image:
        if image.size != (458, 92):
            raise ValueError('Unexpected logo slice size')
        image.convert('RGBA').save(args.output / 'login-logo.png')
    record('login-logo.png', logo, size=[458, 92],
           sourceUrl='https://lanhu-oss-2537-2.lanhuapp.com/' + logo.name)
    login_manifest = args.output / 'login-provenance.json'
    if login_manifest.exists():
        login_records = json.loads(login_manifest.read_text(encoding='utf-8'))
        for entry in login_records:
            if entry['file'] == 'login-background.png':
                entry.update(source=records[0]['sourceUrl'], sourceSize=[3840, 2160],
                             sourceSha256=records[0]['sourceSha256'])
            if entry['file'] == 'login-logo.png':
                entry.pop('crop', None)
                entry.update(source=records[1]['sourceUrl'], sourceSize=[458, 92],
                             sourceSha256=records[1]['sourceSha256'])
        login_manifest.write_text(json.dumps(login_records, indent=2), encoding='utf-8')
    # Coordinates are normalized to the common 1440 x 810 design canvas.
    crops = [
        (1, 'single-load-meter.png', (150, 176, 295, 309)),
        (1, 'single-grid-meter.png', (807, 177, 947, 309)),
        (1, 'single-pcs.png', (420, 490, 580, 598)),
        (1, 'single-battery.png', (420, 691, 580, 795)),
        (2, 'meter.png', (252, 141, 368, 265)),
        (2, 'storage.png', (1060, 140, 1218, 264)),
        (2, 'diesel.png', (229, 497, 389, 624)),
        (7, 'access-lock.png', (674, 295, 843, 426)),
        (11, 'monitor-cells-3.png', (1119, 174, 1179, 234)),
        (14, 'monitor-energy-total-0.png', (622, 698, 664, 740)),
        (14, 'monitor-energy-total-1.png', (813, 698, 855, 740)),
        (14, 'monitor-energy-total-2.png', (1003, 698, 1045, 740)),
        (16, 'alarm-total.png', (1010, 104, 1071, 166)),
        (27, 'control-breaker.png', (178, 370, 220, 412)),
        (29, 'control-diesel-start.png', (178, 370, 220, 412)),
        (29, 'control-grid-breaker.png', (590, 370, 632, 412)),
        (29, 'control-mode-small.png', (1004, 370, 1046, 412)),
        (29, 'control-reset.png', (178, 470, 220, 512)),
        (27, 'command-breaker.png', (459, 384, 475, 397)),
        (29, 'command-auto.png', (459, 484, 475, 500)),
    ]
    for board, name, box in crops:
        source = args.references / f'{board:02d}.png'
        with Image.open(source) as image:
            image.convert('RGB').resize((1440, 810), Image.Resampling.LANCZOS).crop(box).save(args.output / name)
        record(name, source, board=board, crop=list(box))

    with Image.open(args.output / 'storage.png') as storage:
        Image.blend(storage, Image.new('RGB', storage.size, '#E5E9EC'), 0.65).save(args.output / 'storage-idle.png')
    record('storage-idle.png', args.output / 'storage.png', opacity=0.35)
    for i in range(8):
        source = args.references / ('02.png' if i == 1 else '17.png')
        with Image.open(source) as image:
            icon = image.convert('RGB').resize((1440, 810), Image.Resampling.LANCZOS).crop((36, 51 + 93 * i, 70, 81 + 93 * i))
        alpha = icon.convert('L').point(lambda value: min(255, max(0, round((255 - value) * 255 / 101))))
        for state, bg, fg in [('idle', '#FFFFFF', '#959CA6'), ('active', '#129AF5', '#FFFFFF')]:
            name = f'nav-{i}-{state}.png'
            tile = Image.new('RGBA', (82, 76), (0, 0, 0, 0))
            ImageDraw.Draw(tile).rounded_rectangle((0, 0, 81, 75), radius=16, fill=bg)
            colored = Image.new('RGBA', icon.size, fg)
            colored.putalpha(alpha)
            tile.alpha_composite(colored.resize((28, 26), Image.Resampling.LANCZOS), (27, 10))
            tile.save(args.output / name)
            record(name, source, navigationIndex=i, state=state)
    (args.output / 'update-20261016-provenance.json').write_text(json.dumps(records, indent=2), encoding='utf-8')


if __name__ == '__main__':
    main()
