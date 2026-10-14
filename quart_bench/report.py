#!/usr/bin/env python3
"""Print the paper's PostgreSQL VACUUM table from run_replay.sh output.

Usage: report.py [results/replay.csv]

Rows are workloads W1..W9, columns are table scales. "stail over ART" is
RT_SET time / RT_SET_QUART time; "Bulk load over ART" is RT_SET time / guarded
bulk-load time, shown only for streams the guard admits (strictly ascending
ones).
"""
import csv
import sys

WORKLOADS = [
    ("W1", "update"), ("W2", "range25"), ("W3", "range75"),
    ("W4", "striped7"), ("W5", "banded"), ("W6", "sparse"),
    ("W7", "wideband"), ("W8", "mixed"), ("W9", "abort"),
]


def scale_value(label):
    mult = {"M": 1e6, "B": 1e9}.get(label[-1], 1)
    return float(label.rstrip("MB")) * mult


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "results/replay.csv"
    with open(path, newline="") as f:
        rows = list(csv.DictReader(f))

    cells = {(r["workload"], r["scale"]): r for r in rows}
    scales = sorted({r["scale"] for r in rows}, key=scale_value)
    failed = [r for r in rows if r["verify"] != "ok"]

    head = (["Workload", "Density"]
            + [f"stail {s}" for s in scales]
            + [f"bulk {s}" for s in scales])
    print("| " + " | ".join(head) + " |")
    print("|" + "---|" * len(head))
    for wid, wl in WORKLOADS:
        present = [cells.get((wl, s)) for s in scales]
        if not any(present):
            continue
        density = next(float(c["density"]) for c in present if c)
        stail = [f"{float(c['stail_speedup']):.2f}" if c else "" for c in present]
        bulk = [(f"{float(c['bulk_speedup']):.2f}" if c["sorted"] == "yes"
                 else "--") if c else "" for c in present]
        print(f"| {wid} {wl} | {density:.2f} | " + " | ".join(stail + bulk) + " |")

    if failed:
        print(f"\nVERIFY FAILED: {len(failed)} stream(s):", file=sys.stderr)
        for r in failed:
            print(f"  {r['scale']} {r['workload']}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
