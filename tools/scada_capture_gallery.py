"""Build a credential-free Chinese snapshot gallery from verified capture outputs."""
import hashlib
import html
import json
from pathlib import Path
import shutil
import sys
import tarfile
import zipfile
from PIL import Image, ImageDraw, ImageFont, ImageStat

source, actual, output = map(Path, sys.argv[1:4])
output.mkdir(parents=True, exist_ok=False)
report = json.loads((source / 'capture-report.json').read_bytes())
if (source / 'pipeline-report.json').exists():
    pipeline = json.loads((source / 'pipeline-report.json').read_bytes())
else:
    raw_batches = []
    for path in sorted(source.glob('api-response-*.json')):
        raw = path.read_bytes()
        body = json.loads(raw)
        raw_batches.append(dict(file=path.name, sourceTimestampMs=body['ts'], returnedCount=len(body['points']),
                                responseSha256=hashlib.sha256(raw).hexdigest()))
    pipeline = dict(batches=raw_batches, atomicSnapshot=False,
                    sourceTimestampMinMs=min(b['sourceTimestampMs'] for b in raw_batches),
                    sourceTimestampMaxMs=max(b['sourceTimestampMs'] for b in raw_batches),
                    missingSamples=report['missingSampleIndexes'],
                    metadataOrigin='Reconstructed locally from saved raw API batches after download interruption',
                    download=json.loads((source / 'download-report.json').read_bytes()))
    (source / 'pipeline-report.json').write_text(json.dumps(pipeline, indent=2), encoding='utf-8')
names = {p['screenId'] for p in report['pages']}
if len(names) != 18:
    raise ValueError('Expected 18 unique pages')
(output / 'pages').mkdir()
with tarfile.open(source / 'pages.tar.gz') as archive:
    for name in sorted(names):
        member = archive.getmember('pages/' + name + '.png')
        if not member.isfile():
            raise ValueError('PNG member is not a regular file')
        data = archive.extractfile(member).read()
        expected = next(p for p in report['pages'] if p['screenId'] == name)
        if hashlib.sha256(data).hexdigest() != expected['pngSha256']:
            raise ValueError('PNG hash mismatch')
        (output / 'pages' / (name + '.png')).write_bytes(data)
labels = dict(Overview='总览', LocalControl='策略与三相手动功率', Alarms='告警', Maintenance='运维')
labels.update({'Control-Dido': '开关量控制', 'Control-Pcs': 'PCS控制', 'Devices-Bms': '电池设备',
              'Devices-Fire': '消防设备', 'Devices-Pcs': 'PCS设备', 'Devices-Thermal': '温控设备',
              'Strategy-Balance': '均衡策略', 'Strategy-Charge': '充电策略', 'Strategy-Grid': '并网策略',
              'Strategy-Overview': '策略总览', 'Trends-Battery': '电池趋势', 'Trends-Grid': '电网趋势',
              'Trends-Power': '功率趋势', 'Trends-Thermal': '温控趋势'})
order = ['Overview', 'LocalControl', 'Control-Pcs'] + sorted(names - {'Overview', 'LocalControl', 'Control-Pcs'})
font = ImageFont.truetype('C:/Windows/Fonts/msyh.ttc', 22)
sheet = Image.new('RGB', (1920, 6 * 406), '#e7eced')
draw = ImageDraw.Draw(sheet)
qa = []
for i, name in enumerate(order):
    image = Image.open(output / 'pages' / (name + '.png')).convert('RGB')
    if image.size != (1920, 1080):
        raise ValueError('Unexpected page dimensions')
    deviation = sum(ImageStat.Stat(image.resize((192, 108))).stddev)
    if deviation < 5:
        raise ValueError('Possibly blank page: ' + name)
    qa.append(dict(screenId=name, dimensions=list(image.size), pixelDeviation=deviation, hashVerified=True))
    x, y = (i % 3) * 640, (i // 3) * 406
    sheet.paste(image.resize((624, 351)), (x + 8, y + 40))
    draw.text((x + 10, y + 6), labels[name] + ' · snapshot', font=font, fill='#17343c')
sheet.save(output / 'contact-sheet.jpg', quality=92)
for name in ('capture-report.json', 'pipeline-report.json'):
    shutil.copy2(source / name, output / name)
shutil.copy2(actual / 'foreground.png', output / 'actual-overview.png')
shutil.copy2(actual / 'xwd-report.json', output / 'xwd-report.json')
(output / 'pixel-qa.json').write_text(json.dumps(qa, indent=2), encoding='utf-8')
intro = ('COMM202600104 · release-20260912-103919。以下18页为设备端同Qt场景渲染的snapshot，'
         '并非actual framebuffer。按电表分批采样，不是同时刻原子快照；保留原始质量、stale和点时间，'
         'API缺失的expireAt以0表示未知，无历史趋势回放。真实前台XWD总览单独列出。')
cards = ''.join('<figure><a href="pages/' + name + '.png"><img loading="lazy" src="pages/' + name + '.png" alt="' +
                html.escape(labels[name]) + '"></a><figcaption>' + html.escape(labels[name]) +
                ' <small>snapshot · ' + name + '</small></figcaption></figure>' for name in order)
document = '''<!doctype html><html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width">
<title>104部署后画面核验</title><style>body{margin:0;font:16px system-ui,sans-serif;color:#183039;background:#eef2f3;letter-spacing:0}
header,main{max-width:1500px;margin:auto;padding:24px}h1{font-size:26px}p{line-height:1.7}section{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:20px}
figure{margin:0}img{display:block;width:100%;height:auto}figcaption{padding:9px 0}small{color:#4b626b}a{color:#176271}
@media(max-width:800px){section{grid-template-columns:1fr}header,main{padding:14px}}</style><header><h1>104部署后画面核验</h1><p>''' + intro + '''</p>
<a href="contact-sheet.jpg">18页联系图</a> · <a href="说明.md">中文说明</a></header><main><h2>全部18页 snapshot</h2><section>''' + cards + '''</section>
<h2>真实前台总览 · actual framebuffer / XWD</h2><a href="actual-overview.png"><img src="actual-overview.png" alt="真实前台总览"></a></main></html>'''
(output / 'index.html').write_text(document, encoding='utf-8')
note = ('# 104部署后截图\n\n' + intro + '\n\n'
        f'- 页面：18/18，全部1920×1080，PNG哈希与非空检查通过。\n'
        f'- 样本点：{report["sampledPoints"]}；项目路由：{report["routeCount"]}；样本接口未返回：{len(report["missingSampleIndexes"])}，不认定为实际采集丢失。\n'
        f'- 电表批次：{len(pipeline["batches"])}；helper写请求：{report["writeAttempts"]}。\n'
        '- 原始API响应保留于工作证据目录，本交付包不含原配置、账号、TLS材料或凭据。\n'
        '- 已有诊断GET可能执行内部维护，本次不承诺绝对零内部副作用；未改安全开关、未发控制写入、未切前台页面。\n'
        '- 真正前台图为actual-overview.png；pages下均为snapshot，趋势页没有先前进程历史。\n'
        '- 人工画面QA结论另见QA.md。\n')
(output / '说明.md').write_text(note, encoding='utf-8')
with zipfile.ZipFile(output.with_suffix('.zip'), 'w', zipfile.ZIP_DEFLATED) as archive:
    for path in sorted(output.rglob('*')):
        if path.is_file():
            archive.write(path, path.relative_to(output))
print('Gallery and ZIP: ' + str(output))
