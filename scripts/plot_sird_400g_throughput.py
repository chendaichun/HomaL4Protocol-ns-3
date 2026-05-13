#!/usr/bin/env python3
"""Plot Homa/SIRD 400G all-to-all receiver throughput curves."""

from __future__ import annotations

import argparse
from pathlib import Path
from typing import Dict, List, Tuple


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "trace_files",
        nargs="+",
        help="One or more *.throughput.tr files.",
    )
    parser.add_argument(
        "--series",
        default="average_receiver",
        help="Series to plot: average_receiver, aggregate, receiver1, ...",
    )
    parser.add_argument(
        "--labels",
        default="",
        help="Comma-separated legend labels. Defaults to trace stem tags.",
    )
    parser.add_argument(
        "--output",
        required=True,
        help="Output PNG path.",
    )
    parser.add_argument(
        "--title",
        default="Average throughput comparison",
        help="Plot title.",
    )
    parser.add_argument(
        "--time-unit",
        choices=("ms", "s"),
        default="ms",
        help="X-axis time unit.",
    )
    parser.add_argument(
        "--start-zero",
        action="store_true",
        help="Shift the first sample of each trace to t=0.",
    )
    return parser.parse_args()


def parse_fields(line: str) -> Dict[str, str]:
    fields: Dict[str, str] = {}
    for token in line.strip().split():
        if "=" not in token:
            continue
        key, value = token.split("=", 1)
        fields[key] = value
    return fields


def default_label(path: Path) -> str:
    name = path.name
    if name.startswith("sird400g_"):
        name = name[len("sird400g_") :]
    if name.endswith(".throughput.tr"):
        name = name[: -len(".throughput.tr")]
    return name


def load_series(path: Path, series: str, start_zero: bool, time_unit: str) -> List[Tuple[float, float]]:
    rows: List[Tuple[float, float]] = []
    with path.open("r", encoding="utf-8") as f:
        for raw in f:
            raw = raw.strip()
            if not raw:
                continue
            parts = raw.split()
            time_ns = float(parts[0])
            fields = parse_fields(raw)
            if fields.get("series") != series:
                continue
            if "instGbps" not in fields:
                continue
            if time_unit == "ms":
                t = time_ns / 1e6
            else:
                t = time_ns / 1e9
            rows.append((t, float(fields["instGbps"])))

    if start_zero and rows:
        first = rows[0][0]
        rows = [(t - first, y) for t, y in rows]
    return rows


def main() -> int:
    args = parse_args()

    try:
        import matplotlib.pyplot as plt
    except ImportError:
        print("matplotlib is required")
        return 1

    trace_paths = [Path(p) for p in args.trace_files]
    labels = [item.strip() for item in args.labels.split(",") if item.strip()]
    if labels and len(labels) != len(trace_paths):
        raise SystemExit("--labels count must match trace_files count")
    if not labels:
        labels = [default_label(path) for path in trace_paths]

    markers = ["o", "s", "^", "D", "v", "P"]
    colors = ["#1f77b4", "#ff7f0e", "#2ca02c", "#d62728", "#9467bd", "#8c564b"]

    plt.figure(figsize=(8.2, 6.2))
    plotted = 0
    for idx, (path, label) in enumerate(zip(trace_paths, labels)):
        rows = load_series(path, args.series, args.start_zero, args.time_unit)
        if not rows:
            print(f"warning: no rows for series={args.series} in {path}")
            continue
        xs = [x for x, _ in rows]
        ys = [y for _, y in rows]
        plt.plot(
            xs,
            ys,
            label=label,
            linewidth=1.8,
            marker=markers[idx % len(markers)],
            markevery=max(1, len(xs) // 24),
            markersize=4,
            color=colors[idx % len(colors)],
        )
        plotted += 1

    if plotted == 0:
        raise SystemExit("no data plotted")

    plt.title(args.title, fontsize=18)
    plt.xlabel(f"Time ({args.time_unit})", fontsize=15)
    plt.ylabel("Throughput (Gb/s)", fontsize=15)
    plt.grid(True, linestyle="--", alpha=0.28)
    plt.legend(frameon=False, fontsize=11)
    plt.tight_layout()

    out = Path(args.output)
    out.parent.mkdir(parents=True, exist_ok=True)
    plt.savefig(out, dpi=220)
    print(f"output_png: {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
