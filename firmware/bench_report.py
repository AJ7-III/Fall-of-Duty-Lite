#!/usr/bin/env python3
"""Summarize DMA frame selections, pipeline timings and optical latency."""
import argparse
import csv
import json
import math


def read_rows(path):
    with open(path, newline="", encoding="utf-8") as file:
        return [{key: float(value) for key, value in row.items()} for row in csv.DictReader(file)]


def distributions(values):
    ordered = sorted(values)
    if not ordered:
        return {"samples": 0}
    return {
        "samples": len(ordered),
        **{f"p{p}": ordered[math.ceil(len(ordered) * p / 100) - 1] for p in (50, 95, 99)},
        "worst": ordered[-1],
    }


def slot_summary(slots):
    if len(slots) != 3001:
        raise ValueError("expected 3,001 DMA samples including the preceding selection")
    previous = slots[0]
    last_distinct = previous["time_us"]
    misses = run = consecutive = 0
    intervals = []
    for current in slots[1:]:
        if current["time_us"] <= previous["time_us"]:
            raise ValueError("DMA timestamps must increase")
        if current["selected_sequence"] < previous["selected_sequence"]:
            raise ValueError("DMA frame sequence moved backwards")
        if current["selected_sequence"] == previous["selected_sequence"]:
            misses += 1
            run += 1
            consecutive = max(consecutive, run)
        else:
            intervals.append(current["time_us"] - last_distinct)
            last_distinct = current["time_us"]
            run = 0
        previous = current
    elapsed = slots[-1]["time_us"] - slots[0]["time_us"]
    display_underruns = int(slots[-1]["display_underruns"] - slots[0]["display_underruns"])
    if display_underruns < 0:
        raise ValueError("display underrun counter moved backwards")
    return {
        "estimated_panel_hz_from_dma": 2e6 * 3000 / elapsed,
        "scheduled_slots": 3000,
        "distinct_frames": 3000 - misses,
        "misses": misses,
        "consecutive_misses": consecutive,
        "interval_us": distributions(intervals),
        "worst_observed_gap_us": max([slots[-1]["time_us"] - last_distinct, *intervals]),
        "display_underruns": display_underruns,
        "dma_acceptance": misses <= 29 and consecutive <= 1 and display_underruns == 0,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("slots", help="slots.csv copied from the device")
    parser.add_argument("stages", help="stages.csv copied from the device")
    parser.add_argument("--audio-underruns", type=int, help="capture_audio_underruns from a working codec")
    parser.add_argument("--latency", help="CSV of paired optical edges using report_us,photon_us columns")
    args = parser.parse_args()

    slots = read_rows(args.slots)
    report = slot_summary(slots)
    selected = {row["selected_sequence"] for row in slots}
    stages = [row for row in read_rows(args.stages) if row["sequence"] in selected]
    report["selected_frame_stages_us"] = {
        name: distributions([row[name] for row in stages])
        for name in ("setup_us", "raster_us", "world_us", "ppa_us", "hud_us", "submit_us")
    }
    report["audio_underruns"] = args.audio_underruns
    if args.latency:
        latency = [row["photon_us"] - row["report_us"] for row in read_rows(args.latency)]
        if not latency or min(latency) < 0:
            raise ValueError("expected at least one causal report and optical edge pair")
        report["optical_latency_us"] = distributions(latency)
        target = 4e6 / report["estimated_panel_hz_from_dma"]
        report["latency_target_us"] = target
        report["latency_acceptance"] = report["optical_latency_us"]["p95"] <= target
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
