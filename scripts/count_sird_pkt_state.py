#!/usr/bin/env python3
"""
Count total packets, CE-marked packets, and CSN-marked packets
from a SIRD packet-state trace file.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import sys


DEFAULT_TRACE = Path(
    "outputs/sird-scenarios/HomaL4Protocol-1t2-link-test/1t2_default.sird-pkt-state.tr"
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Count total/CE/CSN packets from a .sird-pkt-state.tr file."
    )
    parser.add_argument(
        "trace_file",
        nargs="?",
        default=str(DEFAULT_TRACE),
        help=f"Path to trace file (default: {DEFAULT_TRACE})",
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

    total = 0
    ce_count = 0
    csn_count = 0

    with trace_path.open("r", encoding="utf-8") as f:
        for raw_line in f:
            line = raw_line.strip()
            if not line:
                continue

            fields = parse_fields(line)
            total += 1

            if fields.get("ce") == "1":
                ce_count += 1
            if fields.get("csn") == "1":
                csn_count += 1

    ce_ratio = (ce_count / total) if total else 0.0
    csn_ratio = (csn_count / total) if total else 0.0

    print(f"trace_file: {trace_path}")
    print(f"total_packets: {total}")
    print(f"ce_packets: {ce_count}")
    print(f"csn_packets: {csn_count}")
    print(f"ce_ratio: {ce_ratio:.6f}")
    print(f"csn_ratio: {csn_ratio:.6f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

