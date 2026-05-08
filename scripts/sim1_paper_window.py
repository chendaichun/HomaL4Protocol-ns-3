#!/usr/bin/env python3
"""Derive paper-style sim1 traffic and tracing windows."""

from __future__ import annotations

import argparse
import json
from pathlib import Path


WORKLOAD_DURATION_KNOBS = {
    "google_rpc": 0.09,
    "facebook_hadoop": 0.4,
    "web_search": 0.5,
}


def derive_windows(workload: str, offered_load: float, start_at: float, trace_last_ratio: float) -> dict[str, float]:
    if workload not in WORKLOAD_DURATION_KNOBS:
        raise ValueError(f"unknown workload: {workload}")
    if offered_load <= 0.0:
        raise ValueError("offered_load must be positive")
    if not 0.0 < trace_last_ratio <= 1.0:
        raise ValueError("trace_last_ratio must be in (0, 1]")

    duration_modifier = WORKLOAD_DURATION_KNOBS[workload]
    traffic_duration_sec = duration_modifier / offered_load
    trace_duration_sec = traffic_duration_sec * trace_last_ratio
    trace_start_sec = start_at + traffic_duration_sec - trace_duration_sec

    return {
        "workload": workload,
        "offered_load": offered_load,
        "duration_modifier": duration_modifier,
        "traffic_start_sec": start_at,
        "traffic_duration_sec": round(traffic_duration_sec, 12),
        "trace_start_sec": round(trace_start_sec, 12),
        "trace_duration_sec": round(trace_duration_sec, 12),
        "trace_last_ratio": trace_last_ratio,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="Compute paper-style sim1 time windows")
    parser.add_argument("--workload", required=True, choices=sorted(WORKLOAD_DURATION_KNOBS))
    parser.add_argument("--offered-load", type=float, required=True)
    parser.add_argument("--start-at", type=float, default=10.0)
    parser.add_argument("--trace-last-ratio", type=float, default=0.1)
    parser.add_argument("--json-out", type=Path)
    parser.add_argument("--shell-out", type=Path)
    args = parser.parse_args()

    payload = derive_windows(args.workload, args.offered_load, args.start_at, args.trace_last_ratio)

    if args.json_out:
      args.json_out.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    if args.shell_out:
        args.shell_out.write_text(
            "\n".join(
                [
                    f"TRAFFIC_START_SEC={payload['traffic_start_sec']}",
                    f"TRAFFIC_DURATION_SEC={payload['traffic_duration_sec']}",
                    f"TRACE_START_SEC={payload['trace_start_sec']}",
                    f"TRACE_DURATION_SEC={payload['trace_duration_sec']}",
                    f"ANALYZE_START_SEC={payload['trace_start_sec']}",
                    f"ANALYZE_DURATION_SEC={payload['trace_duration_sec']}",
                ]
            )
            + "\n",
            encoding="utf-8",
        )
    if not args.json_out and not args.shell_out:
        print(json.dumps(payload, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
