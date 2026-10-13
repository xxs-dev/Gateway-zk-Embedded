"""Archive the 60 observed Lanhu boards and produce a reproducible visual inventory."""
import argparse
import hashlib
import json
from pathlib import Path
from PIL import Image, ImageDraw

BOARDS = """
ac0982ea698019c0e4a1872969601041 策略监测
4584646eca84cf2319b40b9022327e91 柴油机监测
892f2a4fe91f8ff5be7272e4e2fcdb11 电表监测
ba797c8b8644b63a15c38b34b3f0bf5d MCCB监测
b8c9e8fe74a4ecdd6f539f6e847bd329 控制未登录
7c3cb541997ef222e05eac77dd40fd80 EMU监测
12b033ed63144c667af53aae621888da 登录（原名仪表）
c6e88ae373bb14e97741ba0885d0e3a9 TMS监测
0db234c1799d492b5a58113d7d9cef25 告警
676b9d3bf9c154b13ce262828e923c67 电芯明细
5feed6babce3fe45b623ad13f967b93c BMS监测
257fadb58b496390c890569ee65045bd 网络监测
808454b408c5f49ff3a007738da3d197 PCS监测（原名EMU）
0138dfdc9f3de6d2ddac68d207c23105 自定义曲线
884330435619534e922042f2abe2b68e 电芯曲线
9404484ef7cf865db0d1bf57a9c590b0 PCS控制
08bec821866ff8693871c9ed12a44c88 MCCB控制
ba66abb537bb4b27facc59eb2ab18f72 策略控制
6933d0e13fd82e1a95dbe3cea96a131c 柴油机控制
6069c3d24e2407781bf291dd496617d5 Frame67
60ea0abf562fd2297ac134caf0492e3b Frame66
b770d84003e335b124f37c40c97ba57f TMS控制
626811bdd00a38171d0dedea66acfd6e BMS控制
0beb8af7b74f482c73e2620eb01263c8 Frame68
c13f46d206e68025f8f3cd44b0b9fd90 Frame70
71af8b3d6be294f65f6d3a05f4bf2965 Frame71
03f3c7549dcf36dbe90b747dc9231e0b 拓扑全部连接
0d253adf91766583180aac8e613b30f4 自检阶段
9bfc1cf733fa0a6abbb612b018414240 仪表
4ae76b3c8d9ae8bb6021dad9e7a5873f 部署正常
756a9ec4a2914a5fe6429b6ff9f84ee7 拓扑1台未连接
06d34b644443a4f350f8bae029e4142d 部署异常
0f92c7eb000fb4a5b4276715cf71b715 Frame64
a914fb34b5ba85901cf8621dec62fa5f 拓扑3台未连接
87de92f826b13882dbec02948f4f99ad 拓扑2台未连接
b775846a98af43047ea0a1c2a9595776 Frame65
8104a8455ccf400379bb51fe6acbcec2 Frame61
5f30ed8de3228d8f4a3e77d7a4fd8f87 仪表旧稿
764dc563084fcc4eeb69d1e959315860 告警旧稿
41f3abb231f91b60f322e91e15f30ba4 自定义曲线旧稿
ea202c0e6a3f19b39e27b7e1314992ab 自检旧稿
1a5c936ba5ecebc27338a95d4e326d2d 拓扑全连接旧稿
4197bdd4fb1810efa842886afa65998a PCS控制旧稿
0f92c7eb000fb4a5b4276715cf71b715 Frame64重复
1a7cc17ca26837068601dceb486fcc7a 部署正常旧稿
7d34c6fcc73b614e174e759982888946 电芯曲线旧稿
c551c3c931f762e754918b2311be8cb6 MCCB控制旧稿
9c0e081273ea9a186fd56628c94f7d1f 策略控制旧稿
ace699a7c69bdb06256bc6b8de5b374b 部署异常旧稿
426e9cadeddc4a26ae00a5a77ca1d08a 柴油机控制旧稿
6069c3d24e2407781bf291dd496617d5 Frame67重复
60ea0abf562fd2297ac134caf0492e3b Frame66重复
b775846a98af43047ea0a1c2a9595776 Frame65重复
293d7ff8ef9bf29d2629d7502d4e37f0 TMS控制旧稿
add6c98ee75f89ae669ddb552ddc4c91 BMS控制旧稿
41753a1625f9f60edf87a211f468adb4 拓扑1台未连接旧稿
278c850209b32a329488992166075113 拓扑2台未连接旧稿
0beb8af7b74f482c73e2620eb01263c8 Frame68重复
6417347a342ab2181dba5a44a91090cb 拓扑3台未连接旧稿
8104a8455ccf400379bb51fe6acbcec2 Frame61重复
""".strip().splitlines()

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--downloads', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    records, images = [], []
    for index, row in enumerate(BOARDS, 1):
        digest, name = row.split(' ', 1)
        source = args.downloads / f'FigmaCover{digest}.png'
        with Image.open(source) as opened:
            img = opened.convert('RGB')
            target = args.output / f'{index:02}.png'
            img.save(target)
            images.append(img.copy())
            records.append(dict(board=index, name=name, asset=f'https://assets.lanhuapp.com/FigmaCover{digest}.png',
                                size=list(img.size), sha256=hashlib.sha256(target.read_bytes()).hexdigest()))
    for start in range(0, 60, 12):
        sheet = Image.new('RGB', (1600, 1040), '#dddddd')
        draw = ImageDraw.Draw(sheet)
        for offset, img in enumerate(images[start:start + 12]):
            x, y = offset % 4 * 400, offset // 4 * 346
            draw.text((x + 8, y + 8), f'BOARD {start + offset + 1:02}', fill='black')
            img.thumbnail((390, 306))
            sheet.paste(img, (x + 5, y + 35))
        sheet.save(args.output / f'contact-{start + 1:02}.jpg')
    (args.output / 'inventory.json').write_text(json.dumps(records, ensure_ascii=False, indent=2), encoding='utf-8')
    print(f'Archived {len(records)} boards: {args.output}')

if __name__ == '__main__':
    main()
