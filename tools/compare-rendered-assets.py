#!/usr/bin/env python3
"""Render identical diagnostic ticks across every asset encoding."""
import argparse
import json
import subprocess
from pathlib import Path
from tempfile import TemporaryDirectory

POSES = {
    "long-lane": [65, 163],
    "apache": [120, 195],
    "weapon-cycle": [80, 262, 346, 578],
    "glass": [35, 146],
}


def pixels(path):
    with path.open("rb") as file:
        if (file.readline(), file.readline(), file.readline()) != (b"P6\n", b"800 480\n", b"255\n"):
            raise ValueError(f"Unexpected image dimensions in {path}")
        result = file.read()
        if len(result) != 800 * 480 * 3:
            raise ValueError(f"Truncated image {path}")
        return result


def compare(a, b):
    changed = 0
    absolute = 0
    maximum = 0
    for i in range(0, len(a), 3):
        if a[i:i + 3] == b[i:i + 3]:
            continue
        changed += 1
        for offset in range(3):
            delta = abs(a[i + offset] - b[i + offset])
            absolute += delta
            maximum = max(maximum, delta)
    return {
        "differentPixels": changed,
        "differentPixelPercent": 100 * changed / (len(a) // 3),
        "meanChannelDelta": absolute / len(a),
        "maxChannelDelta": maximum,
    }


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("--host", required=True, type=Path)
    cli.add_argument("--candidates", required=True, type=Path)
    cli.add_argument("--pack", required=True, type=Path)
    cli.add_argument("--reference", required=True, type=Path)
    cli.add_argument("--out", required=True, type=Path)
    args = cli.parse_args()
    packs = {
        "full": args.candidates / "full.fodp",
        "q16-only": args.candidates / "q16-only.fodp",
        "indexed-only": args.candidates / "indexed-only.fodp",
        "combined": args.pack,
    }
    result = {"comparisons": [], "hostTimingSingleEncoding": {}}
    with TemporaryDirectory(prefix="fod-render-compare-") as scratch:
        paths = {name: Path(scratch) / f"{name}.ppm" for name in packs}
        for scene, marks in POSES.items():
            for tick in marks:
                for mode in (400, 320):
                    for name, pack in packs.items():
                        subprocess.run([str(args.host.resolve()), "--pack", str(pack), "--mode", str(mode),
                                        "--trace", str(args.reference / f"{scene}.fodtrace"), "--image-tick", str(tick),
                                        "--image", str(paths[name])], stdout=subprocess.DEVNULL, check=True)
                    full = pixels(paths["full"])
                    values = {name: compare(full, pixels(path)) for name, path in paths.items() if name != "full"}
                    if values["indexed-only"]["differentPixels"] or values["q16-only"] != values["combined"]:
                        raise ValueError(f"Shot encoding changed a rendered pixel in {scene}, tick {tick}, mode {mode}")
                    result["comparisons"].append({"scene": scene, "tick": tick, "width": mode, **values})
        for name, pack in packs.items():
            result["hostTimingSingleEncoding"][name] = json.loads(subprocess.check_output(
                [str(args.host.resolve()), "--pack", str(pack), "--bench", str(args.reference / "long-lane.fodtrace"),
                 "--mode", "400", "--frames", "600", "--warmup", "120"], text=True
            ))
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(result, indent=2) + "\n")


if __name__ == "__main__":
    main()
