#!/usr/bin/env python3
"""Replay the captured inputs at both sizes and collect repeatable host evidence."""
import argparse
import hashlib
import json
import platform
import subprocess
import sys
from pathlib import Path


def check_campaign(scene, start, ticks):
    mission = [sample["mission"] for sample in ticks]
    phases = sorted({sample["phase"] for sample in mission})
    initial_phase, stages = {
        "breakout": (0, None),
        "landing-zone": (6, None),
        "timberline": (10, range(0, 4)),
        "array": (10, range(4, 10)),
        "array-finale": (10, range(7, 10)),
    }[start]
    valid = mission[0]["phase"] == initial_phase
    if stages is not None:
        valid = (valid and mission[0]["stage"] == stages.start and set(phases) <= {8, 9, 10}
                 and all(sample["stage"] in stages for sample in mission))

    # The checked-in benchmarks have required events. Other traces may record
    # different inputs and can play beyond these scenes.
    if scene == "briar-breakout":
        valid = valid and start == "breakout" and phases == [0, 1, 2] and max(m["aliens"] for m in mission) == 4
    elif scene == "briar-landing-zone":
        valid = (valid and start == "landing-zone" and phases == [6, 7] and any(sample["apache"] for sample in ticks)
                 and any(m["hold"] > 0 and m["perimeter"] == 15 for m in mission))
    elif scene == "briar-finale":
        valid = (valid and start == "array-finale" and phases == [10] and {m["stage"] for m in mission} == {7}
                 and any(m["dish"] for m in mission) and max(m["aliens"] for m in mission) >= 2)
    peak_plasma = max(sample["plasma"] for sample in mission)
    if scene in {"briar-breakout", "briar-landing-zone", "briar-finale"}:
        valid = valid and peak_plasma > 0 and any(sample["shock"] > 0 for sample in mission)
    if not valid:
        raise RuntimeError(f"Campaign replay missed its expected stages or events in {scene}")
    return phases, peak_plasma


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", type=Path, required=True)
    parser.add_argument("--pack", type=Path, required=True)
    parser.add_argument("--reference", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--frames", type=int, default=3000)
    parser.add_argument("--warmup", type=int, default=300)
    parser.add_argument("--skip-bench", action="store_true")
    parser.add_argument("--scene", action="append")
    parser.add_argument("--look", choices=("classic", "enhanced"))
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    host = [str(args.host.resolve()), "--pack", str(args.pack)]
    if args.look:
        host.extend(["--look", args.look])
    manifest = Path(str(args.pack) + ".json")
    base = Path(__file__).resolve().parent
    scenes = args.scene or sorted(path.name.removesuffix(".fodtrace") for path in args.reference.glob("*.fodtrace"))
    if not scenes:
        raise SystemExit("No replay traces found.")
    summary = {
        "clock": "steady_clock wall time on host, uncapped, no live scanout or GPIO",
        "system": platform.platform(),
        "pack": json.loads(subprocess.check_output(host + ["--inspect"], text=True)),
        "scenes": {},
    }

    summary["pack"]["sha256"] = json.loads(manifest.read_text())["sha256"]

    def simulate(options):
        return subprocess.check_output(host + [str(item) for item in options], text=True)

    for scene in scenes:
        trace = args.reference / f"{scene}.fodtrace"
        header, initial, *_ = trace.read_text().splitlines()
        browser = args.reference / f"{scene}.browser.json"
        journal = args.out / f"{scene}.400.jsonl"
        compact = args.out / f"{scene}.320.jsonl"
        result = json.loads(simulate(["--trace", trace, "--journal", journal, "--mode", 400]))
        simulate(["--trace", trace, "--journal", compact, "--mode", 320])
        result["traceSha256"] = hashlib.sha256(trace.read_bytes()).hexdigest()
        result["resolutionJournalsIdentical"] = journal.read_bytes() == compact.read_bytes()
        if not result["resolutionJournalsIdentical"]:
            raise RuntimeError(f"Resolution changed gameplay in {scene}")
        if result["missingDraws"] or result["unusedDraws"]:
            raise RuntimeError(f"Replay random draws changed in {scene}")
        if header in {"FODTRACE 2", "FODTRACE 3"}:
            ticks = [json.loads(line) for line in journal.read_text().splitlines()]
            key = "mission" if header == "FODTRACE 3" else "secondRival"
            final_tick, input_ticks = result["ticks"], result["inputTicks"]
            # Recorded inputs can outlast the simulation. The results screen
            # freezes its tick while the journal continues to record each input.
            if (not 0 < final_tick <= input_ticks
                    or [sample["tick"] for sample in ticks] != [min(i, final_tick) for i in range(1, input_ticks + 1)]
                    or any(key not in sample for sample in ticks)):
                raise RuntimeError(f"Native journal is incomplete in {scene}")
            if key == "mission":
                result["missionPhases"], result["peakPlasma"] = check_campaign(scene, initial.split()[-1], ticks)
        else:
            report = args.out / f"{scene}.comparison.json"
            subprocess.run([sys.executable, str(base / "compare-replay.py"), str(browser), str(journal),
                            "--manifest", str(manifest), "--strict", "--out", str(report)], check=True)
            comparison = json.loads(report.read_text())
            result["firstBrowserDivergenceTick"] = comparison["firstDivergenceTick"]
            result["knownApacheRivalHeightTicks"] = comparison["knownApacheRivalHeightTicks"]
        print(f"{scene}: {result['ticks']} ticks, replay checks passed", flush=True)
        for mark in sorted(args.reference.glob(f"{scene}-t*.png")):
            suffix = mark.stem.rsplit("-t", 1)[1]
            if not suffix.isdecimal():
                continue
            tick = int(suffix)
            for mode in (400, 320):
                output = args.out / f"{mark.stem}.{mode}.ppm"
                simulate(["--trace", trace, "--image-tick", tick, "--image", output, "--mode", mode])
        if (header == "FODTRACE 3" or scene in {"long-lane", "apache", "ffa-apache"}) and not args.skip_bench:
            output = simulate(["--bench", trace, "--frames", args.frames, "--warmup", args.warmup, "--mode", "both"])
            result["hostTiming"] = [json.loads(line) for line in output.splitlines()]
        summary["scenes"][scene] = result
        (args.out / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")


if __name__ == "__main__":
    main()
