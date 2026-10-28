"""Archive downloaded design images and compare them with a pinned earlier review."""
import argparse
import hashlib
import html
import json
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw


def changed_pixels(left, right, box=None):
    if box:
        left, right = left.crop(box), right.crop(box)
    diff = ImageChops.difference(left, right)
    mask = ImageChops.lighter(ImageChops.lighter(*diff.split()[:2]), diff.split()[2])
    mask = mask.point(lambda value: 255 if value > 12 else 0)
    count = mask.histogram()[255]
    return round(100 * count / (mask.width * mask.height), 4), mask.getbbox(), mask


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--inventory', type=Path, required=True)
    parser.add_argument('--downloads', type=Path, required=True)
    parser.add_argument('--previous', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    spec = json.loads(args.inventory.read_text(encoding='utf-8'))
    args.output.mkdir(parents=True, exist_ok=True)
    records, tiles, sections = [], [], []
    for board in spec['boards']:
        number = board['board']
        path = args.downloads / ('FigmaCover' + board['assetDigest'] + '.png')
        raw = path.read_bytes()
        image = Image.open(path).convert('RGB')
        current = args.output / ('%02d.png' % number)
        image.save(current)
        record = dict(board, size=list(image.size), sourceSha256=hashlib.sha256(raw).hexdigest(),
                      sha256=hashlib.sha256(current.read_bytes()).hexdigest())
        record['sourceUrl'] = 'https://assets.lanhuapp.com/' + path.name
        record['boardUrl'] = ('https://lanhuapp.com/web/#/item/project/detailDetach?pid=' +
                              spec['projectId'] + '&image_id=' + board['imageId'])
        ref = board['previousBoard']
        comparison = '<p>New reference</p>'
        if ref is not None:
            old = Image.open(args.previous / ('%02d.png' % ref)).convert('RGB')
            old.save(args.output / ('%02d-before.png' % number))
            before = old.resize(image.size, Image.Resampling.LANCZOS)
            percent, bounds, mask = changed_pixels(before, image)
            record.update(changedPixelPercent=percent, changedBounds=bounds,
                          previousSize=list(old.size))
            mask.save(args.output / ('%02d-diff.png' % number))
            if image.width >= 1900 and image.height >= 1080 and not (board['route'] or '').startswith('Login'):
                body, _, _ = changed_pixels(before, image, (176, 82, 1920, 1080))
                record['bodyChangedPixelPercent'] = body
            comparison = ('<p>Changed pixels: %s%% (diagnostic only, not fidelity acceptance)</p>' % percent +
                          '<img loading="lazy" src="%02d-before.png" alt="Previous design">' % number)
        label = '%02d %s' % (number, board['route'] or board['name'])
        sections.append('<section><h2>%s</h2><p>%s</p><a href="%s">Lanhu</a>%s'
                        '<img loading="lazy" src="%02d.png" alt="Current design"></section>' %
                        (html.escape(label), html.escape(board['name']), html.escape(record['boardUrl']), comparison, number))
        thumb = image.copy()
        thumb.thumbnail((470, 265))
        tiles.append((label, thumb))
        records.append(record)
        print(label, {key: record.get(key) for key in ('changedPixelPercent', 'bodyChangedPixelPercent', 'changedBounds')})
    for start in range(0, len(tiles), 12):
        sheet = Image.new('RGB', (1920, 900), '#eeeeee')
        draw = ImageDraw.Draw(sheet)
        for offset, (label, thumbnail) in enumerate(tiles[start:start + 12]):
            x, y = offset % 4 * 480, offset // 4 * 300
            draw.text((x + 5, y + 5), label, fill='black')
            sheet.paste(thumbnail, (x + 5, y + 28))
        sheet.save(args.output / ('contact-%02d.jpg' % (start + 1)))
    result = dict(spec, boards=records)
    (args.output / 'inventory.json').write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    (args.output / 'index.html').write_text('<!doctype html><meta charset="utf-8"><title>Lanhu reference changes</title>'
        '<style>body{font:16px system-ui;margin:24px;color:#222}section{border-top:1px solid #ccc;padding:16px 0}'
        'img{width:49%;vertical-align:top}a{display:block}h2{font-size:20px}</style>'
        '<h1>EMS 2.0 / ' + html.escape(spec['date']) + '</h1>' + ''.join(sections), encoding='utf-8')


if __name__ == '__main__':
    main()
