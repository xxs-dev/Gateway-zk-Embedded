"""Crop device artwork at source resolution, without baking in telemetry."""
import argparse
import hashlib
import json
from pathlib import Path

from PIL import Image


CROPS = {
    "single-load-meter.png": (150, 176, 295, 309),
    "single-grid-meter.png": (807, 177, 947, 309),
    "single-pcs.png": (420, 490, 580, 598),
    "single-battery.png": (420, 691, 580, 795),
}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--reference", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    records = []
    with Image.open(args.reference) as source:
        if source.width < 1920 or abs(source.width / source.height - 16 / 9) > 0.01:
            raise ValueError("Expected a full-resolution 16:9 design")
        args.output.mkdir(parents=True, exist_ok=True)
        for name, box in CROPS.items():
            native = tuple(round(value * (source.width / 1440 if i % 2 == 0 else source.height / 810))
                           for i, value in enumerate(box))
            target = args.output / name
            picture = source.convert("RGB").crop(native)
            picture.save(target)
            records.append(dict(file=name, designCrop=list(box), nativeCrop=list(native),
                                size=list(picture.size), sha256=hashlib.sha256(target.read_bytes()).hexdigest()))
        manifest = dict(date="2026-10-20", sourceUrl="https://assets.lanhuapp.com/FigmaCoverbaf4a047eb263786ab18dcae50f3f49b.png",
                        sourceSize=list(source.size), sourceSha256=hashlib.sha256(args.reference.read_bytes()).hexdigest(),
                        supersedes="The four single-* entries in update-20261016-provenance.json", assets=records)
    (args.output / "single-topology-20261020-provenance.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")


if __name__ == "__main__":
    main()
