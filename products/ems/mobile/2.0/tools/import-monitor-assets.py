"""Crop only icons from the user-supplied monitor boards; never flatten a page."""
import argparse
import hashlib
import json
from pathlib import Path

from PIL import Image

parser = argparse.ArgumentParser()
parser.add_argument('--references', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
prefix = '凯源移动储能车拓扑监控-监测-'
boards = {
    'network': '网络监测.png', 'emu': '设备监测.png', 'pcs': '设备监测-EMU.png',
    'bms': '设备监测-BMS.png', 'cells': '设备监测-1#电池簇.png',
    'tms': '设备监测-TMS.png', 'mccb': '设备监测MCCB.png',
    'meters': '设备监测电表.png', 'diesel': '设备监测发电机.png',
}
images = {}
sources = {}
for key, filename in boards.items():
    path = args.references / (prefix + filename)
    with Image.open(path) as source:
        if source.size not in ((1920, 1080), (1920, 1081)):
            raise ValueError(f'Unexpected board size: {path}: {source.size}')
        images[key] = source.convert('RGB').resize((1440, 810))
    sources[key] = {'file': path.name, 'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}
crops = []


def crop(name, board, box):
    images[board].crop(box).save(args.output / (name + '.png'))
    crops.append({'asset': name + '.png', 'board': board, 'box1440': box})


for i in range(3):
    x = 153 + i * 429
    crop(f'monitor-network-{i}', 'network', (x, 181, x + 37, 218))
crop('monitor-emu', 'emu', (162, 233, 207, 278))
for i, x in enumerate((187, 442, 637, 809, 992, 1186)):
    crop(f'monitor-emu-info-{i}', 'emu', (x, 317, x + 48, 365))
for i, x in enumerate((200, 509, 818, 1127)):
    crop(f'monitor-emu-resource-{i}', 'emu', (x, 449, x + 54, 503))
crop('monitor-measurement', 'pcs', (185, 278, 207, 300))
crop('monitor-signal', 'pcs', (516, 278, 538, 300))
crop('monitor-alarm', 'pcs', (960, 278, 982, 300))
crop('monitor-energy', 'bms', (607, 564, 630, 587))
crop('monitor-cell-link', 'bms', (203, 720, 245, 762))
for i, (x, y) in enumerate(((194, 543), (358, 543), (194, 624), (358, 624), (194, 706))):
    crop(f'monitor-dc-{i}', 'pcs', (x, y, x + 42, y + 42))
for i, x in enumerate((622, 815, 1008, 1198)):
    crop(f'monitor-energy-{i}', 'bms', (x, 614, x + 42, 656))
for i, x in enumerate((660, 918, 1170)):
    crop(f'monitor-energy-total-{i}', 'bms', (x, 698, x + 42, 740))
for i, x in enumerate((153, 474, 796, 1117)):
    crop(f'monitor-cells-{i}', 'cells', (x, 173, x + 60, 233))
for name, y in (('liquid', 236), ('dehumidifier', 415), ('fan', 521), ('fire', 610)):
    crop('monitor-' + name, 'tms', (180, y, 202, y + 22))
crop('monitor-breaker', 'mccb', (179, 253, 221, 296))
crop('monitor-meter', 'meters', (181, 234, 235, 288))
crop('monitor-diesel', 'diesel', (162, 226, 228, 292))
(args.output / 'monitor-provenance.json').write_text(
    json.dumps({'sources': sources, 'crops': crops}, indent=2, ensure_ascii=False), encoding='utf-8')
print(f'Imported {len(crops)} monitor icons')
