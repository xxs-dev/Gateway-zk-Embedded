"""Check native screenshots and render explicitly synthetic monitor fixtures."""
import argparse
import json
import subprocess
import time
from pathlib import Path

from PIL import Image

parser = argparse.ArgumentParser()
parser.add_argument('--project', type=Path, required=True)
parser.add_argument('--screens', type=Path, required=True)
parser.add_argument('--renderer', type=Path, required=True)
parser.add_argument('--machine-code', required=True)
args = parser.parse_args()
monitor_ids = [
    'Monitor-Network', 'Monitor-Strategy', 'Devices-Emu', 'Devices-Pcs',
    'Devices-Bms', 'Devices-Thermal', 'Devices-Mccb', 'Devices-Meters',
    'Devices-Meters-Page2', 'Devices-Diesel', 'Devices-Cells',
    'Devices-Cells-2', 'Devices-Cells-Page9',
]
checks = []
for path in sorted((args.project / 'screens').glob('*.json')):
    screen = json.loads(path.read_text(encoding='utf-8-sig'))
    png = args.screens / (screen['screenId'] + '.png')
    with Image.open(png) as image:
        if image.size != (1440, 810):
            raise ValueError(f'Wrong dimensions: {png}')
        pixels = image.convert('RGB').crop((132, 155, 1403, 788))
        dark = sum(count for count, pixel in pixels.getcolors(pixels.width * pixels.height) if max(pixel) < 180)
        if screen['screenId'] in monitor_ids and dark < 1000:
            raise ValueError(f'Monitor body appears blank: {png}')
    checks.append({'screen': screen['screenId'], 'dimensions': [1440, 810], 'darkBodyPixels': dark})

tags = json.loads((args.project / 'tags.json').read_text(encoding='utf-8-sig'))
now = int(time.time() * 1000)
overrides = {
    1209: 0, 1210: 0, 1211: 1, 1214: 1, 1216: 1, 1217: 0,
    1399: 1, 3999: 1, 127: 1, 188: 1, 1099: 1, 1199: 1,
    9099: 1, 310000: 1, 920000003: 1, 920000005: 78,
    997: 1, 998: 0, 999: 1, 991: 1, 18: 1, 24: 1, 20: 0, 8: 0,
    1566: 832.6, 1567: -12.5, 1569: 88.2, 1570: 96.3, 1571: 65535,
    1574: 3338, 1577: 3310, 1580: 29, 1583: 25, 1573: 27,
    1576: 12, 1579: 83, 1582: 225, 1585: 48, 588: 120,
}
values_by_unit = {'V': 233.3, 'A': 15.2, 'kW': 25.8, 'kvar': 3.5,
                  'kVA': 26.1, 'kWh': 12345.67, 'kvarh': 3456.7, 'kVarh': 3456.7,
                  'Hz': 50, '℃': 27.5, 'Bar': 1.8, 'ppm': 0.2, '%RH': 45.6,
                  'mV': 3336, 'MiB': 1234.5, 'KiB/s': 145.6}
points = [{'index': tag['indexFallback'],
           'value': overrides.get(tag['indexFallback'], values_by_unit.get(tag.get('unit'), 0)),
           'quality': 1, 'stale': False, 'ts': now, 'expireAt': now + 600000} for tag in tags]
fixture = args.screens / 'monitor-fixture.json'
fixture.write_text(json.dumps({'type': 'pointBatch', 'points': points}, indent=2), encoding='utf-8')
for screen in monitor_ids:
    with (args.screens / (screen + '-fixture-render.log')).open('w', encoding='utf-8') as log:
        subprocess.run([str(args.renderer.resolve()), '--project-dir', str(args.project.resolve()),
                        '--machine-code', args.machine_code, '--screen-id', screen,
                        '--screenshot', str((args.screens / (screen + '-fixture.png')).resolve()),
                        '--preview-values', str(fixture.resolve())], stdout=log, stderr=log, check=True)
(args.screens / 'monitor-render-check.json').write_text(
    json.dumps({'fieldData': False, 'fixtureScreens': monitor_ids, 'screenChecks': checks}, indent=2), encoding='utf-8')
print(f'Checked {len(checks)} native screenshots; rendered {len(monitor_ids)} synthetic fixtures. Not a similarity score.')
