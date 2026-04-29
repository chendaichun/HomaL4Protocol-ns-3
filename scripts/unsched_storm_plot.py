#!/usr/bin/env python3
"""Plot nT1 unscheduled short-message storm traces."""

from __future__ import annotations

import argparse
import csv
import math
import re
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Tuple

import matplotlib.pyplot as plt


CASE_RE = re.compile(r"^n(?P<senders>\d+)_(?P<mode>.+)_load(?P<load>[0-9p.]+)$")


def parse_case_name(path: Path) -> Optional[Tuple[int, str, float]]:
    match = CASE_RE.match(path.name)
    if not match:
        return None
    load = float(match.group("load").replace("p", "."))
    return int(match.group("senders")), match.group("mode"), load


def percentile(values: Iterable[float], pct: float) -> Optional[float]:
    xs = sorted(v for v in values if math.isfinite(v))
    if not xs:
        return None
    if len(xs) == 1:
        return xs[0]
    rank = (len(xs) - 1) * pct / 100.0
    low = int(math.floor(rank))
    high = int(math.ceil(rank))
    if low == high:
        return xs[low]
    frac = rank - low
    return xs[low] * (1.0 - frac) + xs[high] * frac


def parse_kv_line(line: str) -> Dict[str, str]:
    result: Dict[str, str] = {}
    for part in line.strip().split():
        if "=" not in part:
            continue
        key, value = part.split("=", 1)
        result[key] = value.strip('"')
    return result


def parse_summary(path: Path) -> Dict[str, float]:
    result: Dict[str, float] = {
        "receiver_dropped_pkts": 0.0,
        "receiver_dropped_bytes": 0.0,
        "receiver_marked_pkts": 0.0,
        "receiver_marked_bytes": 0.0,
    }
    if not path.exists():
        return result

    with path.open("r", encoding="utf-8") as handle:
        for line in handle:
            stripped = line.strip()
            if not stripped:
                continue
            if stripped.startswith("queue="):
                kv = parse_kv_line(stripped)
                if kv.get("queue") == "switch_port_to_receiver":
                    result["receiver_dropped_pkts"] = float(kv.get("droppedPkts", 0))
                    result["receiver_dropped_bytes"] = float(kv.get("droppedBytes", 0))
                    result["receiver_marked_pkts"] = float(kv.get("markedPkts", 0))
                    result["receiver_marked_bytes"] = float(kv.get("markedBytes", 0))
                continue
            if "=" not in stripped:
                continue
            key, value = stripped.split("=", 1)
            try:
                result[key] = float(value)
            except ValueError:
                pass
    return result


def parse_msg_fcts(path: Path, target_size: int) -> Tuple[int, int, List[float]]:
    starts: Dict[Tuple[str, str, str, int], int] = {}
    started = 0
    finished = 0
    fcts_us: List[float] = []

    if not path.exists():
        return started, finished, fcts_us

    with path.open("r", encoding="utf-8") as handle:
        for line in handle:
            parts = line.split()
            if len(parts) != 6:
                continue
            event, time_ns_s, size_s, src, dst, tx_msg_id = parts
            try:
                time_ns = int(time_ns_s)
                size = int(size_s)
            except ValueError:
                continue
            if size != target_size:
                continue

            key = (src, dst, tx_msg_id, size)
            if event == "+":
                starts[key] = time_ns
                started += 1
            elif event == "-":
                start_ns = starts.pop(key, None)
                if start_ns is None:
                    continue
                finished += 1
                fcts_us.append((time_ns - start_ns) / 1000.0)

    return started, finished, fcts_us


def parse_receiver_queue(path: Path) -> Tuple[List[float], List[float]]:
    times_ms: List[float] = []
    packets: List[float] = []
    if not path.exists():
        return times_ms, packets

    with path.open("r", encoding="utf-8") as handle:
        for line in handle:
            parts = line.split()
            if len(parts) < 4:
                continue
            try:
                time_ns = int(parts[0])
            except ValueError:
                continue
            kv = parse_kv_line(" ".join(parts[1:]))
            if kv.get("queue") != "switch_port_to_receiver":
                continue
            try:
                pkt_count = float(kv["packets"])
            except (KeyError, ValueError):
                continue
            times_ms.append(time_ns / 1e6)
            packets.append(pkt_count)
    return times_ms, packets


def collect_rows(root: Path, short_size: int, long_size: int) -> List[Dict[str, object]]:
    rows: List[Dict[str, object]] = []
    for case_dir in sorted(p for p in root.iterdir() if p.is_dir()):
        parsed = parse_case_name(case_dir)
        if parsed is None:
            continue
        senders, mode, load = parsed
        tag = case_dir.name
        prefix = case_dir / f"unsched_storm_{tag}"
        summary = parse_summary(prefix.with_suffix(".summary.tr"))
        short_started, short_finished, short_fcts = parse_msg_fcts(
            prefix.with_suffix(".msg.tr"), short_size
        )
        long_started, long_finished, long_fcts = parse_msg_fcts(
            prefix.with_suffix(".msg.tr"), long_size
        )
        row: Dict[str, object] = {
            "case": tag,
            "senders": senders,
            "mode": mode,
            "load_gbps": load,
            "short_started": short_started,
            "short_finished": short_finished,
            "short_incomplete": max(0, short_started - short_finished),
            "short_p50_us": percentile(short_fcts, 50),
            "short_p99_us": percentile(short_fcts, 99),
            "short_p999_us": percentile(short_fcts, 99.9),
            "long_started": long_started,
            "long_finished": long_finished,
            "long_incomplete": max(0, long_started - long_finished),
            "long_p50_us": percentile(long_fcts, 50),
            "long_p99_us": percentile(long_fcts, 99),
            "peak_receiver_queue_pkts": summary.get("peakReceiverQueuePkts"),
            "peak_receiver_queue_bytes": summary.get("peakReceiverQueueBytes"),
            "receiver_dropped_pkts": summary.get("receiver_dropped_pkts"),
            "receiver_dropped_bytes": summary.get("receiver_dropped_bytes"),
            "receiver_marked_pkts": summary.get("receiver_marked_pkts"),
            "receiver_marked_bytes": summary.get("receiver_marked_bytes"),
        }
        rows.append(row)
    return rows


def write_summary_csv(rows: List[Dict[str, object]], path: Path) -> None:
    fields = [
        "case",
        "senders",
        "mode",
        "load_gbps",
        "short_started",
        "short_finished",
        "short_incomplete",
        "short_p50_us",
        "short_p99_us",
        "short_p999_us",
        "long_started",
        "long_finished",
        "long_incomplete",
        "long_p50_us",
        "long_p99_us",
        "peak_receiver_queue_pkts",
        "peak_receiver_queue_bytes",
        "receiver_dropped_pkts",
        "receiver_dropped_bytes",
        "receiver_marked_pkts",
        "receiver_marked_bytes",
    ]
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        for row in rows:
            writer.writerow({field: row.get(field) for field in fields})


def grouped(rows: List[Dict[str, object]], senders: int) -> Dict[str, List[Dict[str, object]]]:
    result: Dict[str, List[Dict[str, object]]] = {}
    for row in rows:
        if int(row["senders"]) != senders:
            continue
        result.setdefault(str(row["mode"]), []).append(row)
    for mode_rows in result.values():
        mode_rows.sort(key=lambda r: float(r["load_gbps"]))
    return result


def plot_metric_vs_load(
    rows: List[Dict[str, object]],
    out_path: Path,
    metric: str,
    ylabel: str,
    title: str,
) -> None:
    if not rows:
        return
    senders_values = sorted({int(row["senders"]) for row in rows})
    fig, axes = plt.subplots(
        len(senders_values), 1, figsize=(7.0, 3.5 * len(senders_values)), squeeze=False
    )
    for ax, senders in zip(axes[:, 0], senders_values):
        for mode, mode_rows in sorted(grouped(rows, senders).items()):
            xs = [float(row["load_gbps"]) for row in mode_rows]
            ys = [
                float(row[metric]) if row.get(metric) not in (None, "") else math.nan
                for row in mode_rows
            ]
            ax.plot(xs, ys, marker="o", label=mode)
        ax.set_title(f"{title} (N={senders})")
        ax.set_xlabel("Short-message aggregate offered load (Gbps)")
        ax.set_ylabel(ylabel)
        ax.grid(True, alpha=0.3)
        ax.legend()
    fig.tight_layout()
    fig.savefig(out_path, dpi=180)
    plt.close(fig)


def plot_queue_timeseries(root: Path, rows: List[Dict[str, object]], out_path: Path) -> None:
    if not rows:
        return
    max_sender = max(int(row["senders"]) for row in rows)
    max_load = max(float(row["load_gbps"]) for row in rows if int(row["senders"]) == max_sender)
    selected = [
        row
        for row in rows
        if int(row["senders"]) == max_sender and float(row["load_gbps"]) == max_load
    ]
    if not selected:
        return

    fig, ax = plt.subplots(figsize=(8.0, 4.2))
    for row in sorted(selected, key=lambda r: str(r["mode"])):
        case = str(row["case"])
        prefix = root / case / f"unsched_storm_{case}"
        times_ms, packets = parse_receiver_queue(prefix.with_suffix(".switch-egress-queue.tr"))
        if not times_ms:
            continue
        ax.plot(times_ms, packets, label=str(row["mode"]))
    ax.set_title(f"Receiver-facing queue time series (N={max_sender}, load={max_load:g}Gbps)")
    ax.set_xlabel("Simulation time (ms)")
    ax.set_ylabel("Queue occupancy (packets)")
    ax.grid(True, alpha=0.3)
    ax.legend()
    fig.tight_layout()
    fig.savefig(out_path, dpi=180)
    plt.close(fig)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--out-dir", type=Path, required=True)
    parser.add_argument("--short-size", type=int, default=8192)
    parser.add_argument("--long-size", type=int, default=10000000)
    args = parser.parse_args()

    args.out_dir.mkdir(parents=True, exist_ok=True)
    rows = collect_rows(args.root, args.short_size, args.long_size)
    write_summary_csv(rows, args.out_dir / "unsched_storm_summary.csv")

    plot_metric_vs_load(
        rows,
        args.out_dir / "peak_receiver_queue_vs_load.png",
        "peak_receiver_queue_pkts",
        "Peak receiver queue (packets)",
        "Receiver-facing queue peak",
    )
    plot_metric_vs_load(
        rows,
        args.out_dir / "receiver_drops_vs_load.png",
        "receiver_dropped_pkts",
        "Receiver queue drops (packets)",
        "Receiver-facing queue drops",
    )
    plot_metric_vs_load(
        rows,
        args.out_dir / "short_p99_fct_vs_load.png",
        "short_p99_us",
        "Short-message p99 FCT (us)",
        "Short-message tail latency",
    )
    plot_metric_vs_load(
        rows,
        args.out_dir / "long_p99_fct_vs_load.png",
        "long_p99_us",
        "Long-message p99 FCT (us)",
        "Background long-flow tail latency",
    )
    plot_queue_timeseries(
        args.root,
        rows,
        args.out_dir / "receiver_queue_timeseries_highest_load.png",
    )


if __name__ == "__main__":
    main()
