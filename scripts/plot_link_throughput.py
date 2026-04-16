#!/usr/bin/env python3
"""
Plot per-link throughput time series from a .link-throughput.tr file.
"""

from __future__ import annotations

import argparse
from collections import defaultdict
from pathlib import Path
import sys


DEFAULT_TRACE = Path(
    "outputs/sird-scenarios/HomaL4Protocol-20t1-link-test/20t1_default.link-throughput.tr"
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Plot per-link throughput from a .link-throughput.tr file."
    )
    parser.add_argument(
        "trace_file",
        nargs="?",
        default=str(DEFAULT_TRACE),
        help=f"Path to link-throughput trace file (default: {DEFAULT_TRACE})",
    )
    parser.add_argument(
        "--metric",
        choices=("instGbps", "avgGbps"),
        default="instGbps",
        help="Which throughput metric to plot.",
    )
    parser.add_argument(
        "--prefix",
        default="",
        help="Optional link name prefix filter, e.g. 'sender' or 'switch_to_sender'.",
    )
    parser.add_argument(
        "--output",
        default="",
        help="Optional output PNG path. Defaults next to trace file.",
    )
    return parser.parse_args()


def parse_fields(line: str) -> dict[str, str]:
    fields: dict[str, str] = {}
    for token in line.strip().split():
        if "=" not in token:
            continue
        key, value = token.split("=", 1)
        fields[key] = value
    return fields


def main() -> int:
    args = parse_args()
    trace_path = Path(args.trace_file)

    if not trace_path.exists():
        print(f"error: trace file not found: {trace_path}", file=sys.stderr)
        return 1

    try:
        import matplotlib.pyplot as plt
    except ImportError:
        print("error: matplotlib is required", file=sys.stderr)
        return 1

    series: dict[str, list[tuple[float, float]]] = defaultdict(list)

    with trace_path.open("r", encoding="utf-8") as f:
        for raw_line in f:
            line = raw_line.strip()
            if not line:
                continue

            parts = line.split()
            time_ns = float(parts[0])
            fields = parse_fields(line)
            link = fields.get("link", "")
            if not link:
                continue
            if args.prefix and not link.startswith(args.prefix):
                continue

            metric_value = fields.get(args.metric)
            if metric_value is None:
                continue

            time_s = time_ns / 1e9
            value = float(metric_value)
            series[link].append((time_s, value))

    if not series:
        print("error: no matching link series found", file=sys.stderr)
        return 1

    plt.figure(figsize=(14, 8))
    for link in sorted(series):
        xs = [x for x, _ in series[link]]
        ys = [y for _, y in series[link]]
        plt.plot(xs, ys, linewidth=1.2, label=link)

    plt.xlabel("Time (s)")
    plt.ylabel(f"{args.metric} (Gbps)")
    plt.title(f"Per-link {args.metric}")
    plt.grid(True, alpha=0.3)
    plt.legend(fontsize=8, ncol=2)
    plt.tight_layout()

    if args.output:
        out_path = Path(args.output)
    else:
        suffix = f".{args.metric}"
        if args.prefix:
            suffix += f".{args.prefix}"
        out_path = trace_path.with_suffix(trace_path.suffix + suffix + ".png")

    plt.savefig(out_path, dpi=160)
    print(f"trace_file: {trace_path}")
    print(f"series_count: {len(series)}")
    print(f"metric: {args.metric}")
    print(f"output_png: {out_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

