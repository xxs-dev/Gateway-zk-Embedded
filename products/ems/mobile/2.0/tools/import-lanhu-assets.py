"""Extract approved bitmap assets from the user's Lanhu design exports."""
import argparse
from pathlib import Path

from PIL import Image, ImageDraw

parser = argparse.ArgumentParser()
parser.add_argument("--topology", type=Path, required=True)
parser.add_argument("--meters", type=Path, required=True)
parser.add_argument("--guide", type=Path, required=True)
parser.add_argument("--deploy", type=Path, required=True)
parser.add_argument("--alarms", type=Path, required=True)
parser.add_argument("--strategy", type=Path, required=True)
parser.add_argument("--pcs", type=Path, required=True)
parser.add_argument("--tms", type=Path, required=True)
parser.add_argument("--mccb", type=Path, required=True)
parser.add_argument("--diesel", type=Path, required=True)
parser.add_argument("--cell", type=Path, required=True)
parser.add_argument("--custom", type=Path, required=True)
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
topology = Image.open(args.topology).convert("RGB").resize((1440, 810))
meters = Image.open(args.meters).convert("RGB").resize((1440, 810))
guide = Image.open(args.guide).convert("RGB").resize((1440, 810))
deploy = Image.open(args.deploy).convert("RGB").resize((1440, 810))
alarms = Image.open(args.alarms).convert("RGB").resize((1440, 810))
strategy = Image.open(args.strategy).convert("RGB").resize((1440, 810))
pcs = Image.open(args.pcs).convert("RGB").resize((1440, 810))
tms = Image.open(args.tms).convert("RGB").resize((1440, 810))
mccb = Image.open(args.mccb).convert("RGB").resize((1440, 810))
diesel = Image.open(args.diesel).convert("RGB").resize((1440, 810))
cell = Image.open(args.cell).convert("RGB").resize((1440, 810))
custom = Image.open(args.custom).convert("RGB").resize((1440, 810))
for name, source, box in (
    ("meter-heading", meters, (132, 150, 158, 176)),
    ("chart-heading", meters, (132, 447, 158, 473)),
    ("power-heading", custom, (132, 150, 158, 176)),
    ("cell-voltage-heading", cell, (132, 150, 158, 176)),
    ("cell-temperature-heading", cell, (132, 470, 158, 496)),
    ("metric-0", meters, (802, 239, 840, 277)),
    ("metric-1", meters, (953, 238, 991, 276)),
    ("metric-2", meters, (1105, 238, 1143, 276)),
    ("metric-3", meters, (1257, 238, 1295, 276)),
    ("metric-total", meters, (830, 344, 868, 382)),
    ("control-mode", pcs, (162, 250, 228, 316)),
    ("control-temperature", tms, (180, 565, 208, 595)),
    ("control-breaker", mccb, (178, 370, 220, 412)),
    ("control-diesel-start", diesel, (178, 370, 220, 412)),
    ("control-mode-small", diesel, (1004, 370, 1046, 412)),
    ("control-reset", diesel, (178, 470, 220, 512)),
    ("command-stop", pcs, (452, 427, 468, 443)),
    ("command-run", pcs, (556, 427, 572, 443)),
    ("command-reset", pcs, (659, 427, 675, 443)),
    ("command-edit", pcs, (660, 570, 675, 585)),
    ("command-mode", pcs, (1250, 255, 1270, 275)),
    ("phase-a", pcs, (180, 564, 208, 594)),
    ("phase-b", pcs, (180, 616, 208, 646)),
    ("phase-c", pcs, (180, 668, 208, 698)),
    ("status-normal", guide, (444, 277, 468, 301)),
    ("status-abnormal", guide, (444, 325, 468, 349)),
):
    source.crop(box).save(args.output / f"{name}.png")
for name, source, box in (
    ("guide-network", guide, (153, 183, 215, 247)),
    ("guide-safety", guide, (582, 183, 646, 247)),
    ("guide-function", guide, (1010, 183, 1074, 247)),
    ("guide-footer", guide, (153, 698, 213, 756)),
    ("guide-info", deploy, (153, 698, 213, 756)),
    ("guide-voltage", deploy, (153, 227, 215, 289)),
    ("guide-phase", deploy, (582, 227, 646, 289)),
    ("guide-diesel", deploy, (1010, 227, 1074, 289)),
    ("alarm-today", alarms, (154, 104, 213, 166)),
    ("alarm-current", alarms, (582, 104, 643, 166)),
    ("alarm-total", alarms, (1010, 104, 1071, 166)),
):
    source.crop(box).save(args.output / f"{name}.png")
for i, (x, y) in enumerate(((178, 196), (178, 293), (178, 391), (178, 490),
                            (798, 196), (798, 293), (798, 391))):
    strategy.crop((x, y, x + 43, y + 44)).save(args.output / f"strategy-{i}.png")
for name, box in {
    "logo": (470, 14, 586, 50),
    "meter": (252, 141, 368, 265),
    "storage": (1060, 140, 1218, 264),
    "diesel": (229, 497, 389, 624),
}.items():
    topology.crop(box).save(args.output / f"{name}.png")
storage = topology.crop((1060, 140, 1218, 264))
idle = Image.blend(storage, Image.new("RGB", storage.size, "#E5E9EC"), 0.65)
idle.save(args.output / "storage-idle.png")
for i in range(8):
    # Instrument is selected in meters, topology is selected in topology.
    source = topology if i == 1 else meters
    icon = source.crop((36, 51 + 93 * i, 70, 81 + 93 * i)).convert("RGBA")
    alpha = icon.convert("L").point(lambda value: min(255, max(0, (255 - value) * 2)))
    for state, bg, fg in (("idle", "#FFFFFF", "#959CA6"), ("active", "#129AF5", "#FFFFFF")):
        tile = Image.new("RGBA", (82, 76), (0, 0, 0, 0))
        ImageDraw.Draw(tile).rounded_rectangle((0, 0, 81, 75), radius=16, fill=bg)
        colored = Image.new("RGBA", icon.size, fg)
        colored.putalpha(alpha)
        tile.alpha_composite(colored.resize((28, 26)), (27, 10))
        tile.save(args.output / f"nav-{i}-{state}.png")
