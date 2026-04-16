#!/usr/bin/env python3
"""
Plot sender global bucket trace for the 2T1 scenario.

Example:
  python3 scripts/sird-scenarios/plot_2t1_sender_bucket.py \
        --sim-tag sird
"""

from __future__ import annotations

import argparse
from pathlib import Path
from typing import Dict, List, Tuple

import matplotlib.pyplot as plt


def parse_kv_line(line: str) -> Dict[str, str]:
    parts = line.strip().split()
    if not parts:
        return {}

    row: Dict[str, str] = {"time_ns": parts[0]}
    for token in parts[1:]:
        if "=" not in token:
            continue
        k, v = token.split("=", 1)
        row[k] = v
    return row


def load_sender_bucket(trace_path: Path) -> Tuple[Dict[str, List[float]], Dict[str, List[int]], int, int]:
    times_by_sender: Dict[str, List[float]] = {}
    bucket_by_sender: Dict[str, List[int]] = {}
    grant_events = 0
    data_events = 0

    with trace_path.open("r", encoding="utf-8") as f:
        for raw in f:
            row = parse_kv_line(raw)
            if not row:
                continue

            event = row.get("event", "")
            sender = row.get("sender", "unknown")
            time_ns = int(row.get("time_ns", "0"))
            bucket = int(row.get("senderGlobalAvailPkts", "0"))

            if event == "grant":
                grant_events += 1
            elif event == "data":
                data_events += 1

            t_ms = time_ns / 1e6
            times_by_sender.setdefault(sender, []).append(t_ms)
            bucket_by_sender.setdefault(sender, []).append(bucket)

    return times_by_sender, bucket_by_sender, grant_events, data_events


def main() -> None:
    parser = argparse.ArgumentParser(description="Plot sender global bucket trace for 2T1 scenario")
    parser.add_argument("--sim-tag", default="sird", help="Simulation tag used in trace file name")
    parser.add_argument(
        "--input",
        default=None,
        help=(
            "Optional custom input trace path. "
            "Default: outputs/sird-scenarios/HomaL4Protocol-2t1-link-test/2t1_<simTag>.sird-sender-global-bucket.tr"
        ),
    )
    parser.add_argument(
        "--output",
        default=None,
        help=(
            "Optional output image path. "
            "Default: outputs/sird-scenarios/HomaL4Protocol-2t1-link-test/2t1_<simTag>.sird-sender-global-bucket.png"
        ),
    )
    args = parser.parse_args()

    default_dir = Path("outputs/sird-scenarios/HomaL4Protocol-2t1-link-test")
    trace_path = Path(args.input) if args.input else default_dir / f"2t1_{args.sim_tag}.sird-sender-global-bucket.tr"
    out_path = Path(args.output) if args.output else default_dir / f"2t1_{args.sim_tag}.sird-sender-global-bucket.png"

    if not trace_path.exists():
        raise SystemExit(f"Input trace not found: {trace_path}")

    times_by_sender, bucket_by_sender, grant_events, data_events = load_sender_bucket(trace_path)
    if not times_by_sender:
        raise SystemExit(f"No parsable data in: {trace_path}")

    plt.figure(figsize=(10, 5))
    for sender in sorted(times_by_sender.keys()):
        plt.step(
            times_by_sender[sender],
            bucket_by_sender[sender],
            where="post",
            linewidth=1.5,
            label=sender,
        )

    plt.title("2T1 Sender Global Bucket (Avail Credits) vs Time")
    plt.xlabel("Time (ms)")
    plt.ylabel("senderGlobalAvailPkts")
    plt.grid(alpha=0.3)
    plt.legend(title="Sender", loc="best")

    # Add summary text in the upper-left corner.
    summary = f"grants={grant_events}, data={data_events}"
    plt.gca().text(0.01, 0.99, summary, transform=plt.gca().transAxes, va="top", ha="left")

    out_path.parent.mkdir(parents=True, exist_ok=True)
    plt.tight_layout()
    plt.savefig(out_path, dpi=160)
    print(f"Saved figure: {out_path}")


if __name__ == "__main__":
    main()
