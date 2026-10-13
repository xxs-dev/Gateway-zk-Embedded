"""Generate the requirements document and all-board audit from observed references."""
import argparse
import hashlib
import html
import json
import shutil
from html.parser import HTMLParser
from pathlib import Path
from PIL import Image

# Board numbers follow the observed Lanhu sidebar, including title-only frames.
REQUIREMENTS = [
    (1, 'Monitor-Strategy', '策略监测', '七项策略分两列展示，含执行状态和参数；详情入口保留。', '使用实际运行反馈；周期、光伏跟踪、保电SOC、单相口径未确认，不显示设计示例。详情入口禁用。'),
    (2, 'Devices-Diesel', '柴油机监测', '保留机组信息、发电侧三相量、市电侧、急停、累计运行及告警。', '无真实柴油机点表，全部未知；不借用电网表或车辆急停。'),
    (3, 'Devices-Meters', '电表监测', '双表并排，展示通信、PT/CT、三相电量、总量、累计电能；支持翻页。', '三只已配置电表分两页；PT/CT未知。无功电能使用kvarh，不复制稿中kWh。'),
    (4, 'Devices-Mccb', 'MCCB监测', '八个断路器位置、通信与合分状态、底部分页位置。', '997/998/999仅合位反馈；其他设备及独立通信未知，不伪造在线。'),
    (5, 'Control-LoggedOut', '未登录拦截', '保留公共导航、白色正文、居中“登录后可查看此界面”和“去登录”按钮；登录成功回到请求页面。', '本次新增。未登录不创建受保护正文或写控件；取消、退出、超时均不能放行。适用于既有Guide/Vehicle/Strategy/Control保护前缀。'),
    (6, 'Devices-Emu', 'EMU监测', '基础信息行、通信与平台状态、CPU/内存/磁盘/温度四个摘要。', '无已确认主机监测SHM接口，保留未知；工程版本不冒充主机版本。'),
    (7, 'Login', '独立登录', '按新版蓝色设备背景和右侧白色表单还原，含品牌、账号、密码、登录按钮。支持返回及触摸输入。', '本次新增。复用本地鉴权、失败锁定和会话超时；密码掩码且提交后清空。“记住密码”禁用，待安全存储方案确认。返回与键盘按钮为实际使用补充。'),
    (8, 'Devices-Thermal', 'TMS监测', '液冷、除湿、消防三区，右侧设备告警列表。', '运行反馈与使能分开；消防减光率为%，压力为Bar。仅一个风机继电器，不能显示三台独立状态。'),
    (9, 'Info-Alarms', '告警', '今日/当前/累计摘要、七列表格、每页八条、分页。', '当前数来自真实条件；历史计数、发生/恢复时间无接口时显示--，不能用采样时间替代。'),
    (10, 'Devices-Cells', '电芯明细', '摘要、极值、电压/温度编号明细和分页选择。', '260个单体各9页，最后4个；电压mV。Pack成员关系未确认，使用全部电芯分页；2#簇不复用1#。'),
    (11, 'Devices-Bms', 'BMS监测', '运行/三级报警、总量、电芯极值、电量和簇入口。', '累计高位不是完整累计；簇功率/循环次数未知。报警按同一BMS过滤。'),
    (12, 'Monitor-Network', '网络监测', '4G、有线、GPS三列；网络状态、流量、速率和定位区域。', '4G绑定已确认点；流量是本次连接MiB、速率KiB/s；IP、有线、GPS未知。'),
    (13, 'Devices-Pcs', 'PCS监测', '温度、运行、告警、DC/AC及分相电参。原画板名称误标EMU，功能归PCS。', 'IGBT温度不改称相温；累计交流/直流充放电分别绑定真实kWh点位；并/离网均看各自反馈。'),
    (14, 'Trends-Custom', '自定义曲线', '上方多序列电量、下方功率因数；选择量、图例、缩放和平移。', '功率/电压独立坐标；无数据不画示例线。保留原有时间窗和真实点位。'),
    (15, 'Trends-Cell', '电芯曲线', '上电压、下温度两块等高区域，显示最高/最低曲线。', '实际电芯电压单位mV；蓝色最高值；无数据不画原稿示例。'),
    (16, 'Control-Pcs', 'PCS控制', '模式、启停复位、并离网、三相P/Q设定，布局对齐新版。', '只开放已确认启停复位与1318-1323分相设定；模式和并离网具名命令未确认时禁用。保留确认、范围校验、回执。'),
    (17, 'Control-Mccb', 'MCCB控制', '模式区和八个断路器控制位置，显示真实合分反馈。', '不将合位点当写命令。写入枚举未确认，动作禁用。'),
    (18, 'Strategy-Overview', '策略控制', '七项策略开关按两列排列，真实反馈决定显示状态。', '目前只确认458防逆流使能可写；8/18/20/24等运行点不能当使能点写入。'),
    (19, 'Control-Diesel', '柴油机控制', '模式、停机、遥控快速停机、市电合分、发电合分、手自动。', '真实点表与命令枚举缺失，全部动作禁用。'),
    (22, 'Control-Thermal', 'TMS控制', '模式、液冷/除湿启停、温度阈值设置区域。', '未确认启停命令与阈值写接口，控件保留禁用，不把使能当运行反馈。'),
    (23, 'Control-Bms', 'BMS控制', '模式和复位/上电/下电位置。', '具名命令枚举未确认，禁止按按钮文案猜值。'),
    (27, 'Topology', '并机拓扑：全连接', '六设备位置、配线、流向、功率、断路器、SOC和分相电量。', '仅本机已确认点位接入；从车和柴油机未知。示意线不等于真实连接状态或并机许可。'),
    (28, 'Guide-Setup', '引导：自检', '网络、安全、功能三列检查与汇总及阶段导航。', '当前仅已配置通信反馈可读；完整安全检查、汇总、手动检查接口未接入，不能显示正常汇总或执行部署。'),
    (29, 'Meters-Overview', '仪表', '五位置切换、三相表、摘要、总量和近两小时功率曲线。', '位置1-4使用实际表/PCS点，位置5缺完整设计留空；不复制示例数据。'),
    (30, 'Guide-Deploy', '引导：部署正常', '离网电压、相序、柴油机检查、阶段状态、完成部署。', '正常只是原稿状态样例；安全检查结果无接口时未知，完成部署禁用。'),
    (31, 'Topology', '并机拓扑：1台未连接', '离线车辆卡片与设备图淡化，在线车辆保持正常显示。', '归入同一拓扑状态，不新增菜单。独立车辆成员关系/通信接口尚待接入，不能靠截图固定状态。'),
    (32, 'Guide-Deploy', '引导：部署异常', '异常项使用红色状态并保留重试/检查入口；不得显示可完成部署。', '同一部署页状态，不新增导航；完整异常判据与重试接口待接入，保持未知/禁用。'),
    (34, 'Topology', '并机拓扑：3台未连接', '三个离线设备淡化，未连接线路不表现为带电/有流向。', '多车及柴油机通信未确认，状态组合验收待联调。当前截图为真实数据缺失状态，不是该场景实现证明。'),
    (35, 'Topology', '并机拓扑：2台未连接', '两个离线车辆淡化，其余设备保持真实状态。', '同一拓扑页状态；逐车绑定和过期规则待多车接口确认，不复制本机遥测。'),
]
OLD_TO_NEW = {38:29,39:9,40:14,41:28,42:27,43:16,45:30,46:15,47:17,48:18,49:32,50:19,54:22,55:23,56:31,57:35,59:34}
TITLES = {20:'控制分组标题',21:'告警分组标题',24:'设置分组标题',25:'监测分组标题',26:'登录分组标题',33:'仪表分组标题',36:'引导分组标题',37:'拓扑分组标题',44:'仪表分组标题重复',51:'控制分组标题重复',52:'告警分组标题重复',53:'引导分组标题重复',58:'设置分组标题重复',60:'拓扑分组标题重复'}

class DocumentLinks(HTMLParser):
    def __init__(self):
        super().__init__()
        self.images, self.targets, self.ids = [], [], set()

    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        if 'id' in attrs:
            assert attrs['id'] not in self.ids, 'Duplicate document anchor'
            self.ids.add(attrs['id'])
        if tag == 'img':
            self.images.append(attrs['src'])
        if tag == 'a' and attrs.get('href', '').startswith('#'):
            self.targets.append(attrs['href'][1:])

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--references', type=Path, required=True)
    parser.add_argument('--screens', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    inventory = json.loads((args.references / 'inventory.json').read_text(encoding='utf-8'))
    current = {r[0]:r for r in REQUIREMENTS}
    assert len(inventory) == 60 and len(current) == 29
    assert set(current) | set(OLD_TO_NEW) | set(TITLES) == set(range(1, 61))
    sections, evidence, md = [], [], ['# EMS 2.0 蓝湖更新需求与逐页核对', '', '基线 e753be3；目标 2.0.13-lanhu；保存日期 2026-10-12（America/Los_Angeles）。', '']
    intro = '''<h1>EMS 2.0 蓝湖更新需求与逐页核对</h1>
<p>保存日期：2026-10-12（America/Los_Angeles）。基线：e753be3；本次：2.0.13-lanhu。</p>
<h2>1. 范围与结论</h2><p>已逐张读取蓝湖60张画板：29张当前页面/状态稿、17张旧对照稿、14张分组标题。29张并非29个新增功能；运行包保留56个路由。</p>
<p>本次补齐新版登录与未登录拦截、全局登录态和退出/超时返回。其余23个主页面保留已实现布局并对照最新原稿；另外4张为拓扑离线和部署异常状态。状态接口缺失明确标为待接入，不能宣称这些运行场景已验收。</p>
<h2>2. 全局约束</h2><ul><li>只维护Gateway-zk；不改平台、Windows产品、EMS算法、SHM ABI或设备协议。</li>
<li>1920×1080/1081设计统一映射到1440×810，保留最新分区、顺序、间距与图标。文案/单位按真实点表纠错。</li>
<li>无值、坏质量、过期、非有限数据均不可显示为正常。缺接口显示--，控制预留禁用，不复制样例数值、时刻和成功状态。</li>
<li>监测只读。受保护前缀保持Guide-、Vehicle-、Strategy-、Control-，不因原稿登录按钮而降低原有权限。</li>
<li>设置、单机拓扑、电能页等无完整正文稿的页面继续留空。分组标题不作功能画面实现。</li></ul>
<h2>3. 登录与权限验收</h2><ol><li>公共页面右上角登录打开新版表单；登录成功留在当前页面。受保护页面先展示拦截，成功后进入原目标。</li>
<li>错误密码保留错误提示，不放行；提交后清空密码。失败锁定、超时和角色沿用原鉴权。</li>
<li>取消不改变权限；退出在任何页面可用。会话超时撤销已登录状态，受保护页面回到拦截，旧控制视图销毁。</li>
<li>触摸键盘可切换账号/密码，支持大小写与已有常用符号；物理键盘保留。记住密码位置禁用，不保存凭据。</li></ol>
<h2>4. 逐页需求与证据</h2><p>左图：本次读取的蓝湖原稿；右图：本机Qt实际渲染。运行数据缺失、禁用控件、真实时钟及数据单位不同不等于漏实现，也不能据此宣称95%通过。</p>'''
    md += ['## 范围', '已逐张读取60张画板：29张当前页面/状态稿、17张旧对照稿、14张分组标题。保持56个运行路由。',
           '本次实现新版登录、未登录拦截、全局登录态、退出及超时；其他已有布局对照最新原稿。多车/安全检查状态仍待接口联调。', '',
           '## 通用要求', '只修改Gateway-zk；不改EMS算法、SHM ABI、协议。监测只读；未知显示--，未接入控制禁用。受保护前缀保持Guide-/Vehicle-/Strategy-/Control-。设置、单机拓扑、电能等缺完整稿的页面留空。', '',
           '## 登录验收', '公共登录成功停留原页；受保护页先拦截再回到请求页。取消不放行；错误密码提示且清空；锁定和超时沿用原鉴权。退出或超时撤销权限。触摸键盘可切换账号/密码。记住密码禁用，不持久化凭据。', '']
    for board, route, name, requirement, boundary in REQUIREMENTS:
        ref = args.references / f'{board:02}.png'
        actual = args.screens / f'{route}.png'
        if not actual.is_file():
            raise FileNotFoundError(actual)
        ref_name, actual_name = f'board-{board:02}-reference.png', f'board-{board:02}-actual.png'
        with Image.open(ref) as original, Image.open(actual) as rendered:
            assert rendered.size == (1440, 810), (route, rendered.size)
            scaled = original.convert('RGB').resize((1440, 810), Image.Resampling.LANCZOS)
            scaled.save(args.output / ref_name)
            Image.blend(scaled, rendered.convert('RGB'), 0.5).save(args.output / f'board-{board:02}-overlay.png')
        shutil.copyfile(actual, args.output / actual_name)
        evidence.append(dict(board=board, route=route, referenceSha256=hashlib.sha256(ref.read_bytes()).hexdigest(),
                             actualSha256=hashlib.sha256(actual.read_bytes()).hexdigest(), status='本次新增' if board in (5,7) else '已有布局/接口边界见需求'))
        sections.append(f'<section id="req-{board}"><h3>R{board:02} · {html.escape(name)}</h3><p><b>需求：</b>{html.escape(requirement)}</p><p><b>实现/接口边界：</b>{html.escape(boundary)}</p><p>路由：<code>{route}</code></p><div class="pair"><figure><figcaption>蓝湖原稿 · 画板{board:02}</figcaption><img loading="lazy" src="{ref_name}"></figure><figure><figcaption>本机Qt · 当前配置/无现场数据</figcaption><img loading="lazy" src="{actual_name}"></figure></div><details><summary>50%叠图</summary><img loading="lazy" src="board-{board:02}-overlay.png"></details></section>')
        md += [f'## R{board:02} {name}', f'路由：`{route}`。', f'需求：{requirement}', f'实现/接口边界：{boundary}',
               f'![蓝湖原稿]({ref_name})', f'![本机Qt]({actual_name})', '']
    rows, appendix = [], []
    for record in inventory:
        board = record['board']
        status = '当前稿' if board in current else (f'旧对照稿，对应画板{OLD_TO_NEW[board]:02}' if board in OLD_TO_NEW else '标题，非功能页面')
        name = current[board][2] if board in current else TITLES.get(board, record['name'])
        filename = f'board-{board:02}-original.png'
        shutil.copyfile(args.references / f'{board:02}.png', args.output / filename)
        rows.append(f'<tr><td>{board:02}</td><td><a href="#board-{board}">{html.escape(name)}</a></td><td>{status}</td><td>{record["size"][0]}×{record["size"][1]}</td></tr>')
        appendix.append(f'<details id="board-{board}"><summary>{board:02} · {html.escape(name)} · {status}</summary><p><a href="{html.escape(record["asset"])}">已观察原稿资源</a> · SHA256: <code>{record["sha256"]}</code></p><img loading="lazy" src="{filename}"></details>')
        md += [f'### 画板{board:02} {name}', f'{status}；来源：{record["asset"]}；SHA256：`{record["sha256"]}`。', f'![原稿]({filename})', '']
    acceptance = '''<h2>5. 验收与发布边界</h2><ul>
<li>56路由生成、坐标边界、导航、真实点位/只读约束及包校验必须通过；旧XMind 34路由回归必须通过。</li>
<li>原生鉴权测试覆盖错误密码、锁定、超时、密码清空、键盘、取消、登录后进入原目标、公共页退出和过期撤权。</li>
<li>视觉按每页分区/坐标35%、字体间距20%、图标颜色20%、控件状态15%、交互10%评审；任一已接入完整页面低于95分需返修。真实数据差异应使用受控fixture核验，不能把大面积白底相同当作得分。</li>
<li>95%为目标，当前未完成正式逐页评分，不能标记通过。未接入状态和控件另列，不用禁用状态掩盖缺失功能。</li>
<li>Allwinner AArch64须在192.168.22.11同步并核对固定源码后构建；真机字体、触摸、缩放、启动/切页耗时、CPU/内存尚待验证。</li>
<li>本轮仅本地预览测试，不推送、不合并、不部署、不执行设备控制。</li></ul>
<h2>6. 待补接口</h2><p>独立从车成员关系及通信、柴油机点表、EMU主机资源、有线/GPS/IP、Pack分组、安全自检/部署、告警历史计数与发生/恢复时间、各厂家控制点和枚举、安全凭据存储。每项应补来源、类型、单位、缩放、刷新/过期、读写属性、枚举/范围及权限/回执测试。</p>
<h2>7. 全部60张画板索引</h2>'''
    md += ['## 验收与发布边界', '56路由和旧XMind34路由回归、原生鉴权/控件测试、包校验必须通过。视觉按分区坐标35%、字体间距20%、图标颜色20%、控件状态15%、交互10%逐页验收，已接入完整页目标>=95分。当前未完成正式评分，不宣称95%达标。',
           'Allwinner AArch64在192.168.22.11固定源码构建、真机字体/触摸/性能仍待验证。本轮不推送、不合并、不部署、不下发设备控制。', '',
           '## 待补接口', '独立从车成员/通信、柴油机点表、EMU资源、有线/GPS/IP、Pack分组、安全检查/部署、告警历史/时间、厂家命令与枚举、安全凭据存储。需确认单位、缩放、质量与过期、范围和回执。', '']
    css = 'body{margin:0;background:#fff;color:#303438;font:16px/1.7 "Microsoft YaHei",Arial,sans-serif}main{max-width:1500px;padding:28px;margin:auto}h1{font-size:28px}h2{font-size:23px;margin-top:36px;border-bottom:2px solid #129af5}h3{font-size:20px}section{padding:20px 0;border-bottom:1px solid #ccd6de}nav{display:flex;flex-wrap:wrap;gap:12px}a{color:#087abb}figure{margin:0;min-width:0}img{width:100%;height:auto;display:block}.pair{display:grid;grid-template-columns:1fr 1fr;gap:16px}table{border-collapse:collapse;width:100%}td,th{border:1px solid #ccd6de;padding:8px;text-align:left}th{background:#eef4f8}details{margin:16px 0}summary{cursor:pointer}code{overflow-wrap:anywhere}p{overflow-wrap:anywhere}@media(max-width:900px){.pair{grid-template-columns:1fr}main{padding:16px}}@media print{section{break-before:page}.pair{grid-template-columns:1fr}details{display:none}nav{display:none}}'
    links = ''.join(f'<a href="#req-{r[0]}">R{r[0]:02} {r[2]}</a>' for r in REQUIREMENTS)
    page = '<!doctype html><html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>EMS 2.0 蓝湖更新需求文档</title><style>' + css + '</style><main>' + intro + '<nav>' + links + '</nav>' + ''.join(sections) + acceptance + '<table><thead><tr><th>编号</th><th>页面</th><th>处理结论</th><th>原始尺寸</th></tr></thead><tbody>' + ''.join(rows) + '</tbody></table>' + ''.join(appendix) + '</main></html>'
    (args.output / 'requirements.html').write_text(page, encoding='utf-8')
    (args.output / 'requirements.md').write_text('\n'.join(md), encoding='utf-8')
    (args.output / 'evidence.json').write_text(json.dumps(evidence, ensure_ascii=False, indent=2), encoding='utf-8')
    (args.output / 'inventory.json').write_text(json.dumps(inventory, ensure_ascii=False, indent=2), encoding='utf-8')
    links = DocumentLinks()
    links.feed(page)
    assert len(links.images) == 147 and set(links.targets) <= links.ids
    for name in links.images:
        with Image.open(args.output / name) as image:
            image.verify()
    (args.output / 'document-check.json').write_text(json.dumps(dict(
        boards=60, selectedBoards=29, oldComparisonBoards=17, titleBoards=14,
        validImageLinks=len(links.images), validAnchors=len(links.targets),
        nativeScreensInspected=True, browserHtmlPreview='Not performed: local file URL blocked by browser policy',
        visual95PercentAccepted=False), indent=2), encoding='utf-8')
    print(args.output / 'requirements.html')

if __name__ == '__main__':
    main()
