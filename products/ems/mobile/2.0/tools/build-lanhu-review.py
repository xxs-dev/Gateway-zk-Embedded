"""Build local, same-size reference/render evidence. This is not a similarity score."""
import argparse
import hashlib
import html
import json
from pathlib import Path

from PIL import Image

parser = argparse.ArgumentParser()
parser.add_argument('--references', type=Path, required=True)
parser.add_argument('--monitor-references', type=Path)
parser.add_argument('--screens', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
pairs = [
    ('Meters-Overview', 'da5f9ac1e70cbd2c.png'),
    ('Topology', '9e8a504aa791edc7.png'),
    ('Guide-Setup', '23599ac4a4874ce8.png'),
    ('Guide-Deploy', 'd261875f09c3b072.png'),
    ('Trends-Custom', '12c8b3231468cbd9.png'),
    ('Trends-Cell', '3e90ca3567ae7501.png'),
    ('Info-Alarms', '2c336ab5c85e8b74.png'),
    ('Control-Pcs', 'a087129870c2b684.png'),
    ('Control-Bms', '9648abc4b6e471b2.png'),
    ('Control-Thermal', '5fe0b5dd94c61efc.png'),
    ('Control-Mccb', '6bb78a777ff0dd72.png'),
    ('Control-Diesel', '3d079696fe3b4c22.png'),
    ('Strategy-Overview', '53fc3fd279c38209.png'),
]
titles = {
    'Meters-Overview': '仪表', 'Topology': '并机拓扑',
    'Guide-Setup': '自检阶段', 'Guide-Deploy': '部署阶段',
    'Trends-Custom': '自定义曲线', 'Trends-Cell': '电芯曲线',
    'Info-Alarms': '告警', 'Control-Pcs': 'PCS 控制',
    'Control-Bms': 'BMS 控制', 'Control-Thermal': 'TMS 控制',
    'Control-Mccb': '断路器控制', 'Control-Diesel': '柴油机控制',
    'Strategy-Overview': '策略控制',
}
if args.monitor_references:
    prefix = '凯源移动储能车拓扑监控-监测-'
    monitors = [
        ('Monitor-Network', '网络监测.png', '网络监测'),
        ('Monitor-Strategy', '策略监测.png', '策略监测'),
        ('Devices-Emu', '设备监测.png', 'EMU 监测'),
        ('Devices-Pcs', '设备监测-EMU.png', 'PCS 监测（原稿误标 EMU）'),
        ('Devices-Bms', '设备监测-BMS.png', 'BMS 监测'),
        ('Devices-Cells', '设备监测-1#电池簇.png', '电芯明细'),
        ('Devices-Thermal', '设备监测-TMS.png', 'TMS 监测'),
        ('Devices-Mccb', '设备监测MCCB.png', '断路器监测'),
        ('Devices-Meters', '设备监测电表.png', '电表监测'),
        ('Devices-Diesel', '设备监测发电机.png', '柴油机监测'),
    ]
    for screen, filename, title in monitors:
        pairs.append((screen, args.monitor_references / (prefix + filename)))
        titles[screen] = title
sections = []
evidence = []
markdown = ['# EMS 2.0 蓝湖逐页对照', '',
            '左：蓝湖原稿；右：本机 Qt 实际渲染。无实时数据时显示 --，预留控制保持禁用。',
            '此文件提供对照证据，不将白底像素比例当作 95% 还原度验收。AArch64 和真机未验证。', '']
for screen, filename in pairs:
    ref_path = args.references / filename
    actual_path = args.screens / f'{screen}.png'
    with Image.open(ref_path) as reference, Image.open(actual_path) as actual:
        source_size = reference.size
        if reference.size not in ((1440, 810), (1440, 811), (1920, 1080), (1920, 1081)) or actual.size != (1440, 810):
            raise ValueError(f'{screen}: unexpected screenshot dimensions')
        reference = reference.convert('RGB').resize((1440, 810))
        reference.save(args.output / f'{screen}-reference.png')
        composite = Image.new('RGB', (2880, 810))
        composite.paste(reference.convert('RGB'), (0, 0))
        composite.paste(actual.convert('RGB'), (1440, 0))
        composite.save(args.output / f'{screen}-comparison.jpg', quality=95)
        Image.blend(reference.convert('RGB'), actual.convert('RGB'), 0.5).save(args.output / f'{screen}-overlay.png')
    (args.output / f'{screen}-actual.png').write_bytes(actual_path.read_bytes())
    evidence.append({'screen': screen, 'referenceSha256': hashlib.sha256(ref_path.read_bytes()).hexdigest(),
                     'renderSha256': hashlib.sha256(actual_path.read_bytes()).hexdigest(), 'dimensions': [1440, 810], 'sourceDimensions': list(source_size)})
    title = html.escape(titles[screen])
    sections.append(f'<section id="{screen}"><h2>{title}</h2><div class="pair"><figure><figcaption>蓝湖原稿</figcaption><img src="{screen}-reference.png"></figure><figure><figcaption>本机 Qt 渲染</figcaption><img src="{screen}-actual.png"></figure></div><details><summary>50% 叠图</summary><img src="{screen}-overlay.png"></details></section>')
    markdown += [f'## {titles[screen]}', f'![原稿与本机渲染]({(args.output / f"{screen}-comparison.jpg").resolve().as_posix()})', '']
links = ''.join(f'<a href="#{screen}">{html.escape(titles[screen])}</a>' for screen, _ in pairs)
page = '<!doctype html><html lang="zh-CN"><meta charset="utf-8"><title>EMS 2.0 蓝湖逐页对照</title><style>body{margin:24px;font:14px Arial;color:#303438;background:#eef1f4}h1{font-size:24px}nav{display:flex;gap:12px;flex-wrap:wrap}a{color:#087cb9}section{margin:32px 0;border-top:1px solid #cbd3d9}h2{font-size:18px}figure{margin:0;min-width:0}figcaption{padding:8px 0}img{display:block;width:100%;height:auto}.pair{display:grid;grid-template-columns:1fr 1fr;gap:12px}details{margin-top:12px}details img{max-width:1440px}summary{cursor:pointer}@media(max-width:900px){.pair{grid-template-columns:1fr}}</style><h1>EMS 2.0 蓝湖逐页对照</h1><p>1440 × 810 原稿与本机 Qt 实际渲染。无数据显示 --，未接入控制保持禁用；此证据不等于 95% 验收通过。AArch64、真机和现场控制未验证。</p><nav>' + links + '</nav>' + ''.join(sections) + '</html>'
(args.output / 'index.html').write_text(page, encoding='utf-8')
(args.output / 'review.md').write_text('\n'.join(markdown), encoding='utf-8')
(args.output / 'evidence.json').write_text(json.dumps(evidence, indent=2), encoding='utf-8')
print(args.output.resolve() / 'index.html')
