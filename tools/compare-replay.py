#!/usr/bin/env python3
"""Compare a deterministic browser capture with a desktop or firmware journal."""
import argparse
import json
import math
from collections import defaultdict
from pathlib import Path


NATIVE_EVENTS = {
    0: "fire", 1: "reload-start", 2: "reload-round", 3: "reload-end", 4: "bolt-start",
    5: "bolt-end", 6: "swap", 12: "kill", 14: "playerDeath", 15: "respawn",
    18: "uav", 20: "laptop-open", 22: "strike-mark", 25: "apache-launch", 27: "match-end",
}
RAY_TYPES = {8: "world", 9: "glass", 10: "glass", 11: "rival"}


def comparisons(browser, native, surfaces):
    errors = defaultdict(list)
    maxima = defaultdict(float)
    browser_events = defaultdict(list)
    native_events = defaultdict(list)
    browser_hits, native_hits = [], []
    browser_dead = False
    limits = browser["tolerance"]

    def equal(key, tick, ref, actual, same):
        if not same:
            errors[key].append({"tick": tick, "browser": ref, "native": actual})

    for ref, actual in zip(browser["ticks"], native):
        tick = ref["tick"]
        equal("tick", tick, tick, actual["tick"], tick == actual["tick"])
        for key in ["gun", "clip", "reserve", "score", "dead", "device", "apache"]:
            equal(key, tick, ref[key], actual[key], ref[key] == actual[key])
        equal("gunState", tick, ref["gunState"], actual["gunState"], ref["gunState"] == actual["gunState"])
        glass = int(actual["glass"], 16)
        equal("glass", tick, ref["glass"], glass, ref["glass"] == glass)
        for key, limit in [("position", "position"), ("rival", "position"), ("camera", "position"),
                           ("real", "time"), ("scaled", "time"), ("scale", "time"),
                           ("yaw", "angle"), ("pitch", "angle"), ("eye", "position"),
                           ("hp", None), ("rivalHp", None)]:
            if key not in ref or key not in actual:
                continue
            a, b = ref[key], actual[key]
            if isinstance(a, list):
                difference = math.dist(a, b)
            elif key == "yaw":
                difference = abs(math.remainder(a - b, math.tau))
            else:
                difference = abs(a - b)
            maxima[key] = max(maxima[key], difference)
            equal(key, tick, a, b, difference <= (limits[limit] if limit else 0.005))

        kinds = [event[0] for event in ref["events"]]
        # Controlled damage calls the player directly and bypasses the
        # shooter's browser event. Compare the authoritative death edge.
        if ref["dead"] and not browser_dead and "playerDeath" not in kinds:
            kinds.append("playerDeath")
        browser_dead = bool(ref["dead"])
        for kind in kinds:
            browser_events[kind].append(tick)
        if "uav" in ref["effects"]:
            browser_events["uav"].append(tick)
        for kind, surface, _ in actual["events"]:
            if name := NATIVE_EVENTS.get(kind):
                native_events[name].append(tick)
        for kind, surface, _ in ref["hits"]:
            browser_hits.append((tick, kind, surface if kind in ["world", "glass"] else None))
        player_events = actual["events"]
        # World/actor damage from bots and streaks can follow the player's
        # ray this tick. The first shot-result event after Fire belongs to it.
        for i, (kind, _, _) in enumerate(player_events):
            if kind != 0:
                continue
            result = next(((RAY_TYPES[k], surfaces[s] if k != 11 and s < len(surfaces) else None)
                           for k, s, _ in player_events[i + 1:] if k in RAY_TYPES), ("miss", None))
            native_hits.append((tick, *result))

    def changes(expected, actual):
        records = []
        for i in range(max(len(expected), len(actual))):
            a = expected[i] if i < len(expected) else None
            b = actual[i] if i < len(actual) else None
            if a != b:
                records.append((a, b))
        return records

    event_differences = {
        kind: {"first": diff[0], "browserTicks": browser_events[kind], "nativeTicks": native_events[kind]}
        for kind in sorted(browser_events.keys() | native_events.keys())
        if (diff := changes(browser_events[kind], native_events[kind]))
    }
    ray_differences = changes(browser_hits, native_hits)
    first_state = min((values[0]["tick"] for values in errors.values()), default=None)
    first_event = min((t for values in event_differences.values() for t in values["first"] if t is not None), default=None)
    first_ray = min((ray[0] for a, b in ray_differences for ray in (a, b) if ray is not None), default=None)
    return {
        "scene": browser["scene"],
        "schema": 1,
        "browserReplayIdentical": browser["browserReplayIdentical"],
        "browserTicks": len(browser["ticks"]),
        "nativeTicks": len(native),
        "tolerance": limits,
        "firstDivergenceTick": min((t for t in [first_state, first_event, first_ray] if t is not None), default=None),
        "fields": {key: {"mismatches": len(values), "first": values[0]} for key, values in errors.items()},
        "maximumAbsoluteDifference": dict(maxima),
        "events": event_differences,
        "rays": {"browser": browser_hits, "native": native_hits, "firstDifference": ray_differences[0] if ray_differences else None},
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("browser", type=Path)
    parser.add_argument("native", type=Path)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--out", type=Path)
    parser.add_argument("--strict", action="store_true", help="Return nonzero on any recorded divergence.")
    args = parser.parse_args()
    browser = json.loads(args.browser.read_text())
    native = [json.loads(line) for line in args.native.read_text().splitlines() if line.strip()]
    surfaces = json.loads(args.manifest.read_text())["shotSurfaces"]
    report = comparisons(browser, native, surfaces)
    result = json.dumps(report, indent=2) + "\n"
    if args.out:
        args.out.write_text(result)
    else:
        print(result, end="")
    if args.strict and (report["firstDivergenceTick"] or report["browserTicks"] != report["nativeTicks"]):
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
