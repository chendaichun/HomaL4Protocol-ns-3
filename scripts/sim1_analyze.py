#!/usr/bin/env python3
"""Extract paper-aligned sim1 summaries from trace files."""

from __future__ import annotations

import argparse
import csv
from pathlib import Path
from typing import Dict, Iterable, List, Tuple


def parse_kv(tokens: Iterable[str]) -> Dict[str, str]:
    values: Dict[str, str] = {}
    for token in tokens:
        if "=" not in token:
            continue
        key, value = token.split("=", 1)
        values[key] = value
    return values


def parse_goodput_trace(
    path: Path, default_num_hosts: int, start_sec: float, duration_sec: float
) -> Tuple[float, float, float, float, int]:
    max_per_host = 0.0
    sum_per_host = 0.0
    mean_total_transport = 0.0
    samples = 0
    active_end_sec = start_sec + duration_sec
    start_completed_bytes = 0.0
    end_completed_bytes = 0.0
    have_start_baseline = False
    total_transport_sum = 0.0

    with path.open("r", encoding="utf-8") as handle:
        for line in handle:
            parts = line.split()
            if not parts:
                continue
            time_sec = int(parts[0]) / 1e9
            fields = parse_kv(parts[1:])
            completed_bytes = float(fields.get("requestCompletedBytes", fields.get("completedBytes", 0.0)))
            if time_sec <= start_sec:
                start_completed_bytes = completed_bytes
                have_start_baseline = True
            if time_sec <= active_end_sec:
                end_completed_bytes = completed_bytes
            if "perHostGoodputGbps" in fields:
                per_host = float(fields["perHostGoodputGbps"])
            else:
                if "requestGoodputGbps" in fields:
                    aggregate = float(fields["requestGoodputGbps"])
                elif "aggregateGoodputGbps" in fields:
                    aggregate = float(fields["aggregateGoodputGbps"])
                elif "goodputGbps" in fields:
                    aggregate = float(fields["goodputGbps"])
                else:
                    continue
                num_hosts = int(fields.get("numHosts", default_num_hosts))
                if num_hosts <= 0:
                    continue
                per_host = aggregate / num_hosts

            if time_sec < start_sec or time_sec > active_end_sec:
                continue
            max_per_host = max(max_per_host, per_host)
            sum_per_host += per_host
            total_transport_sum += float(fields.get("totalTransportGoodputGbps", fields.get("aggregateGoodputGbps", 0.0)))
            samples += 1

    mean_per_host = sum_per_host / samples if samples else 0.0
    mean_total_transport = total_transport_sum / samples if samples else 0.0
    if not have_start_baseline:
        start_completed_bytes = 0.0
    run_avg_per_host = 0.0
    if duration_sec > 0 and default_num_hosts > 0:
        run_avg_per_host = (
            (end_completed_bytes - start_completed_bytes) * 8.0
            / duration_sec
            / 1e9
            / default_num_hosts
        )
    return max_per_host, mean_per_host, run_avg_per_host, mean_total_transport, samples


def parse_queue_trace(path: Path, start_sec: float, duration_sec: float) -> Tuple[float, float, int]:
    max_mb = 0.0
    sum_mb = 0.0
    samples = 0
    active_end_sec = start_sec + duration_sec

    with path.open("r", encoding="utf-8") as handle:
        for line in handle:
            parts = line.split()
            if not parts:
                continue
            time_sec = int(parts[0]) / 1e9
            fields = parse_kv(parts[1:])
            if fields.get("queue") == "aggregate":
                continue

            if "peakBytes" in fields:
                queue_mb = float(fields["peakBytes"]) / 1_000_000.0
                max_mb = max(max_mb, queue_mb)
                continue

            if time_sec < start_sec or time_sec > active_end_sec:
                continue
            if "bytes" not in fields:
                continue

            queue_mb = float(fields["bytes"]) / 1_000_000.0
            max_mb = max(max_mb, queue_mb)
            sum_mb += queue_mb
            samples += 1

    mean_mb = sum_mb / samples if samples else 0.0
    return max_mb, mean_mb, samples


def split_tag(tag: str) -> Tuple[str, str]:
    prefixes = ("balanced_", "core_", "incast_")
    for prefix in prefixes:
        if tag.startswith(prefix):
            return prefix[:-1], tag[len(prefix) :]
    return "", tag


def collect_tags(trace_dir: Path) -> List[str]:
    tags = set()
    for path in trace_dir.glob("sim1_*.goodput.tr"):
        name = path.name
        tags.add(name[len("sim1_") : -len(".goodput.tr")])
    for path in trace_dir.glob("sim1_*.tor-egress-queue.tr"):
        name = path.name
        tags.add(name[len("sim1_") : -len(".tor-egress-queue.tr")])
    return sorted(tags)


def main() -> int:
    parser = argparse.ArgumentParser(description="Analyze sim1 traces with paper-aligned goodput semantics.")
    parser.add_argument("--trace-dir", type=Path, required=True)
    parser.add_argument("--out-csv", type=Path, required=True)
    parser.add_argument("--num-hosts", type=int, default=144)
    parser.add_argument("--start-sec", type=float, default=0.2)
    parser.add_argument("--duration-sec", type=float, default=1.0)
    args = parser.parse_args()

    tags = collect_tags(args.trace_dir)
    rows: List[Dict[str, object]] = []
    for tag in tags:
        traffic_config, workload = split_tag(tag)
        goodput_path = args.trace_dir / f"sim1_{tag}.goodput.tr"
        queue_path = args.trace_dir / f"sim1_{tag}.tor-egress-queue.tr"

        max_goodput, mean_goodput, run_avg_goodput, mean_total_transport_goodput, goodput_samples = (
            0.0,
            0.0,
            0.0,
            0.0,
            0,
        )
        if goodput_path.exists():
            max_goodput, mean_goodput, run_avg_goodput, mean_total_transport_goodput, goodput_samples = parse_goodput_trace(
                goodput_path, args.num_hosts, args.start_sec, args.duration_sec
            )

        max_queue_mb, mean_queue_mb, queue_samples = (0.0, 0.0, 0)
        if queue_path.exists():
            max_queue_mb, mean_queue_mb, queue_samples = parse_queue_trace(
                queue_path, args.start_sec, args.duration_sec
            )

        rows.append(
            {
                "tag": tag,
                "traffic_config": traffic_config,
                "workload": workload,
                "paper_max_goodput_gbps": f"{max_goodput:.6f}",
                "paper_mean_sample_goodput_gbps": f"{mean_goodput:.6f}",
                "paper_run_avg_goodput_gbps": f"{run_avg_goodput:.6f}",
                "paper_mean_total_transport_goodput_gbps": f"{mean_total_transport_goodput:.6f}",
                "paper_max_tor_queue_mb": f"{max_queue_mb:.6f}",
                "paper_mean_tor_queue_mb": f"{mean_queue_mb:.6f}",
                "goodput_samples": goodput_samples,
                "queue_samples": queue_samples,
            }
        )

    args.out_csv.parent.mkdir(parents=True, exist_ok=True)
    with args.out_csv.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(
            handle,
            fieldnames=[
                "tag",
                "traffic_config",
                "workload",
                "paper_max_goodput_gbps",
                "paper_mean_sample_goodput_gbps",
                "paper_run_avg_goodput_gbps",
                "paper_mean_total_transport_goodput_gbps",
                "paper_max_tor_queue_mb",
                "paper_mean_tor_queue_mb",
                "goodput_samples",
                "queue_samples",
            ],
        )
        writer.writeheader()
        for row in rows:
            writer.writerow(row)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
