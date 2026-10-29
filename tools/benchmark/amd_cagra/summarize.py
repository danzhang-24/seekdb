#!/usr/bin/env python3
"""Summarize the paired build-time measurements.

Reads results.txt lines of "<label> <seconds>" and prints the median per
variant and the speedup of the candidate over the frozen baseline, the way the
contest scores it: a weighted geometric mean over the two datasets.
"""
import math
import statistics
import sys
from collections import defaultdict


def parse(path):
    data = defaultdict(list)
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            parts = line.split()
            if len(parts) != 2:
                continue
            try:
                data[parts[0]].append(float(parts[1]))
            except ValueError:
                continue
    return data


def median_of(labels):
    """labels like sift_baseline_r1 -> group key sift_baseline"""
    return labels


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: summarize.py <results.txt>", file=sys.stderr)
        return 2
    data = parse(sys.argv[1])

    groups = defaultdict(list)
    for label, times in data.items():
        for t in times:
            groups[label].append(t)

    print("=== per-run build times (seconds, incl. first query) ===")
    for label in sorted(groups):
        ts = groups[label]
        print("  %-26s n=%d  median=%.3f  min=%.3f  max=%.3f"
              % (label, len(ts), statistics.median(ts), min(ts), max(ts)))

    # dataset -> {variant: median}
    per_ds = defaultdict(dict)
    for label, ts in groups.items():
        if label.endswith("_baseline_r1") or "_baseline_" in label:
            pass
    for ds in ("sift", "hidden"):
        for variant in ("baseline", "candidate"):
            vals = [statistics.median(v) for k, v in groups.items()
                    if k.startswith(ds + "_" + variant + "_")]
            if vals:
                per_ds[ds][variant] = statistics.median(vals)

    print()
    print("=== medians per dataset ===")
    speedups = {}
    for ds, d in sorted(per_ds.items()):
        if "baseline" in d and "candidate" in d and d["baseline"] > 0:
            sp = d["baseline"] / d["candidate"]
            speedups[ds] = sp
            print("  %-8s baseline=%8.3fs  candidate=%8.3fs  speedup=%.3fx"
                  % (ds, d["baseline"], d["candidate"], sp))

    if speedups:
        # contest weighting: SIFT250K 40%, Hidden250K 60%
        w = {"sift": 0.4, "hidden": 0.6}
        acc = 0.0
        wsum = 0.0
        for ds, sp in speedups.items():
            acc += w.get(ds, 0.0) * math.log(sp)
            wsum += w.get(ds, 0.0)
        combined = math.exp(acc / wsum) if wsum else float("nan")
        score = min(max(combined, 1.0) and combined, 3.0)
        print()
        print("  weighted geometric mean build speedup: %.3fx" % combined)
        print("  (contest: 3.0x -> full 14 build points)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
