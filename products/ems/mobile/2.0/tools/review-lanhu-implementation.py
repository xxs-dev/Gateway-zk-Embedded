"""Build an offline design/runtime comparison from verified native Qt evidence."""
import argparse
import hashlib
import html
import json
from datetime import date
from pathlib import Path
import os

from PIL import Image, ImageDraw, ImageStat


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--references', type=Path, required=True)
    parser.add_argument('--evidence', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--date', type=date.fromisoformat)
    parser.add_argument('--requirements', type=Path)
    parser.add_argument('--summary', default='登录采用“储能车＋数据大屏”；当前候选未提交、未部署。')
    args = parser.parse_args()
    refs = json.loads((args.references / 'inventory.json').read_text(encoding='utf-8'))
    audit = json.loads((args.evidence / 'layout-audit.json').read_text(encoding='utf-8'))
    tests = json.loads((args.evidence.parent / 'result.json').read_text(encoding='utf-8'))
    if tests['results'] != {'access': 0, 'behavior': 0, 'audit': 0}:
        raise ValueError('Native behavior/access tests have not passed')
    if audit['renders'] != 448 or audit['issues']:
        raise ValueError('Native layout audit has not passed')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    saved_date = str(args.date or refs['date'])
    notes = {
        1: '已实现单机拓扑；三路断路器使用独立反馈。母线独立测量、DC 接触器及功率方向未确认，保留未知。右图为隔离测试数据，不是实机读数。',
        2: '更新设备图像。实际车辆数由成员信息决定，保留分页，不复制本机数据到其他车。',
        3: '对应三台成员中一台离线的原生场景。',
        4: '对应三台成员中两台离线的原生场景。',
        5: '对应三台成员全部离线的原生场景。',
        7: '补齐文件夹锁图。未登录时不创建受保护控制，登录按钮可用。',
        8: '分区改为居中灰色标题带。保留真实单位、一个风机继电器以及未知接口。',
        10: '太阳能板备选稿，不采用；仅留作设计版本记录。',
        11: '更新功率图标；仍为 260 个单体点、9 页分页，第二簇和 Pack 分组不伪造。',
        14: '累计充电量、累计放电量及循环次数改成三块独立色块。',
        16: '更新累计告警图标，表内使用真实告警来源，不写入设计稿示例告警。',
        19: 'DC/AC 移至概览底部，调整上下布局与连接线。维持 PCS 归属和真实 IGBT 点名。',
        27: '更新断路器与命令图标、行间距和字重；控制接口仍禁用预留。',
        28: '示例开关文案变化不作为控制点表，不新增未确认使能命令。',
        29: '更新柴油机四种操作图标；缺少正式控制接口，按钮仍禁用。',
        38: '用户选定储能车＋数据大屏背景，使用独立背景和透明 Logo 切图，保留原生登录表单、触摸键盘及权限逻辑。',
    }
    specials = {1: 'Topology-Single-state-0.png', 2: 'Topology-3-offline-0.png',
                3: 'Topology-3-offline-1.png', 4: 'Topology-3-offline-2.png',
                5: 'Topology-3-offline-3.png', 7: 'Control-LoggedOut.png', 38: 'Login.png'}
    nav, sections, records = [], [], []

    def figure(path, caption):
        if not path.is_file():
            raise FileNotFoundError(path)
        with Image.open(path) as image:
            if max(ImageStat.Stat(image.convert('RGB')).stddev) < 3:
                raise ValueError('Blank evidence: ' + str(path))
        url = Path(os.path.relpath(path, args.output.parent)).as_posix()
        return ('<figure><figcaption>' + html.escape(caption) + '</figcaption><a href="' + html.escape(url) +
                '"><img loading="lazy" src="' + html.escape(url) + '" alt="' + html.escape(caption) + '"></a></figure>')

    for board in refs['boards']:
        number, route = board['board'], board['route']
        label = f"{number:02d} {board['name']}"
        nav.append(f'<a href="#b{number}">{html.escape(label)}</a>')
        note = notes.get(number, '主体与既有设计无实质变化；同步新版导航，继续保持真实数据、权限和预留接口约束。')
        if route is None:
            note = '分组标题画板，无完整业务页面；不据此虚构界面，缺图页面保持留空。'
        reference = args.references / f'{number:02d}.png'
        images = figure(reference, '蓝湖当前稿')
        actual = None
        if route and route != 'Login-Alternative':
            actual = args.evidence / specials.get(number, route + '-missing.png')
            images += figure(actual, 'AArch64 原生 Qt · 隔离验证截图')
        sections.append(f'<section id="b{number}"><h2>{html.escape(label)}</h2><p>{html.escape(note)}</p>'
                        f'<a href="{html.escape(board["boardUrl"])}">打开对应蓝湖画板</a><div class="comparison">{images}</div></section>')
        records.append(dict(board=number, route=route, decision=note,
                            screenshot=actual.name if actual else None,
                            screenshotSha256=hashlib.sha256(actual.read_bytes()).hexdigest() if actual else None))
    expected = {f'{screen.stem}-{sample}.png' for screen in (args.evidence.parent.parent / 'project/screens').glob('*.json')
                for sample in ('missing', 'values')}
    if len(expected) != 112 or any(not (args.evidence / name).is_file() for name in expected):
        raise ValueError('Incomplete 56-page screenshot coverage')
    extras = ''.join(figure(args.evidence / name, name) for name in [
        'Topology-0-offline-0.png', 'Topology-1-offline-0.png', 'Topology-2-offline-0.png',
        'Topology-5-offline-0.png', 'Topology-5-page2.png', 'Topology-stale.png',
        'Topology-Single-state-1.png', 'Topology-Single-state-2.png', 'Topology-Single-state-3.png',
        'Topology-Single-state-4.png', 'Topology-Single-state-5.png', 'Login-800x600.png'])
    result = dict(date=saved_date, referenceDate=refs['date'], baseCommit=refs['sourceCommit'], renders=audit['renders'],
                  layoutIssues=len(audit['issues']), fullPageReferences=sum(bool(b['route']) for b in refs['boards']),
                  titleReferences=sum(not b['route'] for b in refs['boards']), screenshots=len(expected),
                  visual95PercentAccepted=False, deployed=False, boards=records)
    args.output.with_suffix('.json').write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    document = '''<!doctype html><html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>EMS 2.0 蓝湖更新核对 · __SAVED_DATE__</title>
<style>*{box-sizing:border-box}body{margin:0;font:16px/1.65 "Microsoft YaHei",sans-serif;color:#272b30;background:#fff;letter-spacing:0}
header{padding:28px 32px;background:#edf4f8;border-bottom:1px solid #c7d9e6}h1{font-size:28px;margin:0 0 10px}h2{font-size:21px;margin:0 0 8px}
main{padding:0 28px}p{max-width:1100px}a{color:#0765a2;overflow-wrap:anywhere}nav{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:4px 18px;padding:24px 0}
section{padding:24px 0;border-top:1px solid #cbd5dc;scroll-margin-top:12px}.comparison,.extras{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:16px}
figure{margin:12px 0;min-width:0}figcaption{color:#525b64;font-size:14px}img{width:100%;height:auto;display:block;border:1px solid #d8e0e5}.notice{border-left:4px solid #bd7914;padding-left:16px}
@media(max-width:850px){nav{grid-template-columns:repeat(2,minmax(0,1fr))}.comparison,.extras{grid-template-columns:1fr}header,main{padding:18px}h1{font-size:24px}}
@media print{nav{display:none}section{break-inside:avoid}.comparison{grid-template-columns:repeat(2,minmax(0,1fr))}}</style></head><body>
<header><h1>EMS 2.0 蓝湖更新核对</h1><p>保存日期：__SAVED_DATE__ · 31 张完整页面/状态/备选稿 + 8 张分组标题 · 程序 56 个路由</p>
<p>__SUMMARY__</p>
<p>56 页生成、只读来源、旧 34 页兼容回归、AArch64 登录与交互测试通过。四种分辨率、缺值/长值共 448 次原生渲染及等比缩放检查通过；1920×1080 下逐文本越界/重叠扫描 0 项。</p>
<p class="notice">右图来自隔离测试程序：数值是测试夹具或未知状态，不是现场数据。灰色按钮表示接口预留。自动检查不能替代实屏验收，本轮未宣称达到 95% 或生产级。</p>
<p>__LINKS__</p></header><main>'''
    links = []
    def link(path, label):
        url = Path(os.path.relpath(path, args.output.parent)).as_posix()
        return '<a href="' + html.escape(url) + '">' + html.escape(label) + '</a>'
    if args.requirements:
        if not args.requirements.is_file():
            raise FileNotFoundError(args.requirements)
        links.append(link(args.requirements, '本轮需求与实现记录'))
    links.append(link(args.references / 'index.html', '设计参考与上轮差异'))
    links.append(link(args.output.with_suffix('.json'), '证据索引'))
    document = (document.replace('__SAVED_DATE__', html.escape(saved_date))
                .replace('__SUMMARY__', html.escape(args.summary)).replace('__LINKS__', ' · '.join(links)))
    document += '<nav>' + ''.join(nav) + '</nav>' + ''.join(sections)
    document += '<section><h2>附录：动态成员、失效状态及不同尺寸</h2><div class="extras">' + extras + '</div></section></main></body></html>'
    args.output.write_text(document, encoding='utf-8')
    picks = [1, 7, 8, 14, 19, 27, 29, 38]
    sheet = Image.new('RGB', (1000, 1200), '#f1f3f5')
    draw = ImageDraw.Draw(sheet)
    for i, number in enumerate(picks):
        record = next(r for r in records if r['board'] == number)
        with Image.open(args.evidence / record['screenshot']) as source:
            thumbnail = source.convert('RGB')
            thumbnail.thumbnail((490, 270))
            x, y = i % 2 * 500, i // 2 * 300
            draw.text((x + 5, y + 3), record['route'], fill='#272b30')
            sheet.paste(thumbnail, (x + 5, y + 26))
    sheet.save(args.output.parent / 'implementation-contact.jpg')
    print(json.dumps({key: value for key, value in result.items() if key != 'boards'}))


if __name__ == '__main__':
    main()
