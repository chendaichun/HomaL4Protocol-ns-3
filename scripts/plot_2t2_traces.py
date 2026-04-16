#!/usr/bin/env python3
"""
One-stop plotting script for 2t2 trace outputs.

Default input:
  outputs/sird-scenarios/HomaL4Protocol-2t2-link-test/2t2_default.*.tr

Figures:
1) RTT over time
2) FCT CDF
3) FCT CDF by message size
4) Switch queue over time
5) Per-link throughput over time

If any trace file is missing or has no content, the related figure is skipped.
"""

from __future__ import annotations

import argparse
import math
import os
import tempfile
from collections import defaultdict
from pathlib import Path
from typing import Dict, Iterable, List, Tuple

if "MPLCONFIGDIR" not in os.environ:
    mpl_cache_dir = Path(tempfile.gettempdir()) / "matplotlib-cache"
    mpl_cache_dir.mkdir(parents=True, exist_ok=True)
    os.environ["MPLCONFIGDIR"] = str(mpl_cache_dir)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Plot 2t2 trace figures and summary stats.")
    parser.add_argument(
        "--scenario",
        default="2t2",
        help="Scenario short name used in file prefix (default: 2t2)",
    )
    parser.add_argument(
        "--sim-tag",
        default="default",
        help="Simulation tag used in file prefix (default: default)",
    )
    parser.add_argument(
        "--base-dir",
        default="outputs/sird-scenarios",
        help="Base output directory for scenarios.",
    )
    parser.add_argument(
        "--output-dir",
        default="",
        help="Directory for generated figures/stats (default: <scenario_dir>/plots).",
    )
    parser.add_argument(
        "--max-size-groups",
        type=int,
        default=12,
        help="Max message-size groups to draw in FCT-by-size figure (default: 12).",
    )
    return parser.parse_args()


def parse_kv_tokens(tokens: Iterable[str]) -> Dict[str, str]:
    fields: Dict[str, str] = {}
    for tok in tokens:
        if "=" not in tok:
            continue
        k, v = tok.split("=", 1)
        fields[k] = v
    return fields


def percentile(values: List[float], q: float) -> float:
    if not values:
        return float("nan")
    if len(values) == 1:
        return values[0]
    vals = sorted(values)
    pos = (len(vals) - 1) * q
    lo = int(math.floor(pos))
    hi = int(math.ceil(pos))
    if lo == hi:
        return vals[lo]
    frac = pos - lo
    return vals[lo] * (1.0 - frac) + vals[hi] * frac


def summarize(values: List[float]) -> Dict[str, float]:
    if not values:
        return {"mean": float("nan"), "p95": float("nan"), "p99": float("nan")}
    mean = sum(values) / len(values)
    return {"mean": mean, "p95": percentile(values, 0.95), "p99": percentile(values, 0.99)}


def parse_msg_trace(msg_path: Path) -> List[Dict[str, object]]:
    if not msg_path.exists():
        return []

    starts: Dict[Tuple[str, int, str, int, int], Tuple[int, int]] = {}
    records: List[Dict[str, object]] = []

    with msg_path.open("r", encoding="utf-8") as f:
        for raw in f:
            line = raw.strip()
            if not line:
                continue
            parts = line.split()
            if len(parts) < 6:
                continue

            sign = parts[0]
            try:
                t_ns = int(parts[1])
                size_b = int(parts[2])
                saddr, sport_s = parts[3].split(":")
                daddr, dport_s = parts[4].split(":")
                sport = int(sport_s)
                dport = int(dport_s)
                tx_msg_id = int(parts[5])
            except ValueError:
                continue

            key = (saddr, sport, daddr, dport, tx_msg_id)
            if sign == "+":
                starts[key] = (t_ns, size_b)
            elif sign == "-":
                start = starts.pop(key, None)
                if start is None:
                    continue
                begin_ns, msg_size_b = start
                if t_ns < begin_ns:
                    continue
                records.append(
                    {
                        "sender": saddr,
                        "receiver": daddr,
                        "sport": sport,
                        "dport": dport,
                        "tx_msg_id": tx_msg_id,
                        "msg_size_b": msg_size_b,
                        "begin_ns": begin_ns,
                        "finish_ns": t_ns,
                        "fct_ns": t_ns - begin_ns,
                    }
                )
    return records


def parse_rtt_trace(path_rtt_path: Path, homa_rtt_path: Path, ping_rtt_path: Path) -> Tuple[List[Dict[str, object]], str]:
    source = ""
    path = None
    if path_rtt_path.exists() and path_rtt_path.stat().st_size > 0:
        source = "path-rtt"
        path = path_rtt_path
    elif ping_rtt_path.exists() and ping_rtt_path.stat().st_size > 0:
        source = "ping-rtt"
        path = ping_rtt_path
    elif homa_rtt_path.exists() and homa_rtt_path.stat().st_size > 0:
        source = "homa-rtt"
        path = homa_rtt_path
    else:
        return [], source

    rows: List[Dict[str, object]] = []
    with path.open("r", encoding="utf-8") as f:
        for raw in f:
            line = raw.strip()
            if not line:
                continue
            parts = line.split()
            try:
                t_ns = int(parts[0])
            except (ValueError, IndexError):
                continue
            fields = parse_kv_tokens(parts[1:])
            sender = fields.get("sender", "")
            receiver = fields.get("receiver", "")
            rtt_ns_s = fields.get("rttNs")
            if not sender or not receiver or rtt_ns_s is None:
                continue
            try:
                rtt_ns = int(rtt_ns_s)
            except ValueError:
                continue
            sport = int(fields.get("sport", "0"))
            dport = int(fields.get("dport", "0"))
            flow = (
                f"{sender}:{sport}->{receiver}:{dport}"
                if sport > 0 or dport > 0
                else f"{sender}->{receiver}"
            )
            rows.append(
                {
                    "time_ns": t_ns,
                    "sender": sender,
                    "receiver": receiver,
                    "sport": sport,
                    "dport": dport,
                    "flow": flow,
                    "rtt_ns": rtt_ns,
                }
            )
    return rows, source


def parse_queue_trace(path: Path, value_key: str) -> Dict[str, List[Tuple[float, float]]]:
    if not path.exists() or path.stat().st_size == 0:
        return {}
    series: Dict[str, List[Tuple[float, float]]] = defaultdict(list)
    with path.open("r", encoding="utf-8") as f:
        for raw in f:
            line = raw.strip()
            if not line:
                continue
            parts = line.split()
            try:
                t_s = int(parts[0]) / 1e9
            except (ValueError, IndexError):
                continue
            fields = parse_kv_tokens(parts[1:])
            qname = fields.get("queue", "")
            val_s = fields.get(value_key, "")
            if not qname or not val_s:
                continue
            try:
                val = float(val_s)
            except ValueError:
                continue
            series[qname].append((t_s, val))
    return series


def simplify_queue_series(
    series: Dict[str, List[Tuple[float, float]]], min_dt_s: float = 1e-6
) -> Dict[str, List[Tuple[float, float]]]:
    simplified: Dict[str, List[Tuple[float, float]]] = {}
    for qname, pts in series.items():
        if not pts:
            continue
        ordered = sorted(pts, key=lambda x: x[0])

        # Multiple queue events can share the same timestamp; keep final state at that time.
        dedup: List[Tuple[float, float]] = []
        cur_t, cur_v = ordered[0]
        for t_s, val in ordered[1:]:
            if t_s == cur_t:
                cur_v = val
            else:
                dedup.append((cur_t, cur_v))
                cur_t, cur_v = t_s, val
        dedup.append((cur_t, cur_v))

        if min_dt_s <= 0 or len(dedup) <= 2:
            simplified[qname] = dedup
            continue

        sampled: List[Tuple[float, float]] = [dedup[0]]
        last_keep_t, last_keep_v = dedup[0]
        for t_s, val in dedup[1:-1]:
            if val != last_keep_v or (t_s - last_keep_t) >= min_dt_s:
                sampled.append((t_s, val))
                last_keep_t, last_keep_v = t_s, val
        if dedup[-1] != sampled[-1]:
            sampled.append(dedup[-1])

        simplified[qname] = sampled
    return simplified


def parse_link_throughput(path: Path) -> Dict[str, List[Tuple[float, float, float]]]:
    if not path.exists() or path.stat().st_size == 0:
        return {}
    series: Dict[str, List[Tuple[float, float, float]]] = defaultdict(list)
    with path.open("r", encoding="utf-8") as f:
        for raw in f:
            line = raw.strip()
            if not line:
                continue
            parts = line.split()
            try:
                t_s = int(parts[0]) / 1e9
            except (ValueError, IndexError):
                continue
            fields = parse_kv_tokens(parts[1:])
            link = fields.get("link", "")
            inst = fields.get("instGbps")
            avg = fields.get("avgGbps")
            if not link or inst is None or avg is None:
                continue
            try:
                inst_gbps = float(inst)
                avg_gbps = float(avg)
            except ValueError:
                continue
            series[link].append((t_s, inst_gbps, avg_gbps))
    return series


def plot_rtt_timeseries(rows: List[Dict[str, object]], out_path: Path):
    if not rows:
        return False
    import matplotlib.pyplot as plt

    by_flow: Dict[str, List[Tuple[float, float]]] = defaultdict(list)
    for r in rows:
        by_flow[str(r["flow"])].append((float(r["time_ns"]) / 1e9, float(r["rtt_ns"]) / 1e6))

    if not by_flow:
        return False

    plt.figure(figsize=(12, 7))
    for flow in sorted(by_flow):
        pts = sorted(by_flow[flow], key=lambda x: x[0])
        xs = [x for x, _ in pts]
        ys = [y for _, y in pts]
        plt.plot(xs, ys, marker="o", markersize=2.5, linewidth=1.0, label=flow)
    plt.xlabel("Time (s)")
    plt.ylabel("RTT (ms)")
    plt.title("RTT Over Time")
    plt.grid(alpha=0.3)
    plt.legend(fontsize=8, ncol=2)
    plt.tight_layout()
    plt.savefig(out_path, dpi=160)
    plt.close()
    return True


def cdf_xy(values: List[float]) -> Tuple[List[float], List[float]]:
    vals = sorted(values)
    n = len(vals)
    ys = [(i + 1) / n for i in range(n)]
    return vals, ys


def plot_fct_cdf(fct_ms: List[float], out_path: Path):
    if not fct_ms:
        return False
    import matplotlib.pyplot as plt

    xs, ys = cdf_xy(fct_ms)
    plt.figure(figsize=(10, 6))
    plt.plot(xs, ys, linewidth=1.5)
    plt.xlabel("FCT (ms)")
    plt.ylabel("CDF")
    plt.title("FCT CDF")
    plt.grid(alpha=0.3)
    plt.tight_layout()
    plt.savefig(out_path, dpi=160)
    plt.close()
    return True


def plot_fct_by_size_mean(records: List[Dict[str, object]], out_path: Path, max_groups: int):
    if not records:
        return False
    import matplotlib.pyplot as plt

    by_size: Dict[int, List[float]] = defaultdict(list)
    for r in records:
        by_size[int(r["msg_size_b"])].append(float(r["fct_ns"]) / 1e6)
    if not by_size:
        return False

    ordered_sizes = sorted(by_size.keys())
    if len(ordered_sizes) > max_groups:
        ordered_sizes = sorted(ordered_sizes, key=lambda s: len(by_size[s]), reverse=True)[:max_groups]
        ordered_sizes = sorted(ordered_sizes)

    xs = [size_b for size_b in ordered_sizes]
    ys = [sum(by_size[size_b]) / len(by_size[size_b]) for size_b in ordered_sizes]

    plt.figure(figsize=(12, 7))
    plt.plot(xs, ys, marker="o", linewidth=1.5)
    plt.xlabel("Message Size (Bytes)")
    plt.ylabel("Mean FCT (ms)")
    plt.title("Mean FCT By Message Size")
    plt.grid(alpha=0.3)
    if len(xs) > 1:
        plt.xscale("log")
    plt.tight_layout()
    plt.savefig(out_path, dpi=160)
    plt.close()
    return True


def plot_switch_queue(
    pkts_series: Dict[str, List[Tuple[float, float]]],
    bytes_series: Dict[str, List[Tuple[float, float]]],
    out_path: Path,
):
    if not pkts_series and not bytes_series:
        return False
    import matplotlib.pyplot as plt

    if pkts_series and bytes_series:
        fig, axes = plt.subplots(2, 1, figsize=(13, 9), sharex=True)
        ax0, ax1 = axes
    else:
        fig, ax0 = plt.subplots(1, 1, figsize=(13, 6))
        ax1 = None

    if pkts_series:
        pkts_ymax = 0.0
        for q in sorted(pkts_series):
            pts = sorted(pkts_series[q], key=lambda x: x[0])
            if pts:
                pkts_ymax = max(pkts_ymax, max(y for _, y in pts))
            ax0.plot(
                [x for x, _ in pts],
                [y for _, y in pts],
                linewidth=1.0,
                drawstyle="steps-post",
                label=q,
            )
        ax0.set_ylabel("Queue Packets")
        ax0.set_ylim(0.0, max(1.0, pkts_ymax * 1.1))
        ax0.set_title("Switch Queue Occupancy Over Time")
        ax0.grid(alpha=0.3)
        ax0.legend(fontsize=8, ncol=2)

    if bytes_series:
        tgt = ax1 if ax1 is not None else ax0
        bytes_ymax = 0.0
        for q in sorted(bytes_series):
            pts = sorted(bytes_series[q], key=lambda x: x[0])
            if pts:
                bytes_ymax = max(bytes_ymax, max(y for _, y in pts))
            tgt.plot(
                [x for x, _ in pts],
                [y for _, y in pts],
                linewidth=1.0,
                drawstyle="steps-post",
                label=q,
            )
        tgt.set_ylabel("Queue Bytes")
        tgt.set_ylim(0.0, max(1.0, bytes_ymax * 1.1))
        tgt.set_xlabel("Time (s)")
        tgt.grid(alpha=0.3)
        if ax1 is not None:
            tgt.legend(fontsize=8, ncol=2)
        elif not pkts_series:
            tgt.set_title("Switch Queue Occupancy Over Time")
            tgt.legend(fontsize=8, ncol=2)
    else:
        ax0.set_xlabel("Time (s)")

    fig.tight_layout()
    fig.savefig(out_path, dpi=160)
    plt.close(fig)
    return True


def plot_link_throughput(series: Dict[str, List[Tuple[float, float, float]]], out_path: Path):
    if not series:
        return False
    import matplotlib.pyplot as plt

    plt.figure(figsize=(13, 7))
    for link in sorted(series):
        pts = sorted(series[link], key=lambda x: x[0])
        xs = [x for x, _, _ in pts]
        inst = [y for _, y, _ in pts]
        plt.plot(xs, inst, linewidth=1.2, label=link)
    plt.xlabel("Time (s)")
    plt.ylabel("Instant Throughput (Gbps)")
    plt.title("Per-link Throughput Over Time")
    plt.grid(alpha=0.3)
    plt.legend(fontsize=8, ncol=2)
    plt.tight_layout()
    plt.savefig(out_path, dpi=160)
    plt.close()
    return True


def write_stats_file(
    out_path: Path,
    fct_stats: Dict[str, float],
    rtt_by_flow_stats: Dict[str, Dict[str, float]],
    rtt_source: str,
):
    lines = []
    lines.append("FCT summary (ms):")
    lines.append(
        f"  mean={fct_stats['mean']:.6f} p95={fct_stats['p95']:.6f} p99={fct_stats['p99']:.6f}"
        if not math.isnan(fct_stats["mean"])
        else "  no data"
    )
    lines.append("")
    lines.append(f"RTT summary by flow (source={rtt_source or 'none'}, ms):")
    if not rtt_by_flow_stats:
        lines.append("  no data")
    else:
        for flow in sorted(rtt_by_flow_stats):
            s = rtt_by_flow_stats[flow]
            lines.append(
                f"  {flow} mean={s['mean']:.6f} p95={s['p95']:.6f} p99={s['p99']:.6f}"
            )
    out_path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    args = parse_args()

    scenario_dir = Path(args.base_dir) / f"HomaL4Protocol-{args.scenario}-link-test"
    prefix = f"{args.scenario}_{args.sim_tag}"

    msg_path = scenario_dir / f"{prefix}.msg.tr"
    path_rtt_path = scenario_dir / f"{prefix}.path-rtt.tr"
    homa_rtt_path = scenario_dir / f"{prefix}.homa-rtt.tr"
    ping_rtt_path = scenario_dir / f"{prefix}.ping-rtt.tr"
    q_pkts_path = scenario_dir / f"{prefix}.switch-queue-pkts.tr"
    q_bytes_path = scenario_dir / f"{prefix}.switch-queue-bytes.tr"
    link_thr_path = scenario_dir / f"{prefix}.link-throughput.tr"

    out_dir = Path(args.output_dir) if args.output_dir else (scenario_dir / "plots")
    out_dir.mkdir(parents=True, exist_ok=True)

    records = parse_msg_trace(msg_path)
    fct_ms = [float(r["fct_ns"]) / 1e6 for r in records]
    fct_stats = summarize(fct_ms)

    rtt_rows, rtt_source = parse_rtt_trace(path_rtt_path, homa_rtt_path, ping_rtt_path)
    rtt_by_flow: Dict[str, List[float]] = defaultdict(list)
    for r in rtt_rows:
        rtt_by_flow[str(r["flow"])].append(float(r["rtt_ns"]) / 1e6)
    rtt_by_flow_stats = {flow: summarize(vals) for flow, vals in rtt_by_flow.items()}

    q_pkts_series = simplify_queue_series(parse_queue_trace(q_pkts_path, "newPackets"))
    q_bytes_series = simplify_queue_series(parse_queue_trace(q_bytes_path, "newBytes"))
    link_series = parse_link_throughput(link_thr_path)

    generated = []
    skipped = []

    try:
        import matplotlib  # noqa: F401
    except ImportError:
        print("skip all plots: matplotlib not installed")
        write_stats_file(out_dir / "summary_stats.txt", fct_stats, rtt_by_flow_stats, rtt_source)
        return 0

    if plot_rtt_timeseries(rtt_rows, out_dir / "01_rtt_timeseries.png"):
        generated.append("01_rtt_timeseries.png")
    else:
        skipped.append("01_rtt_timeseries.png")

    if plot_fct_cdf(fct_ms, out_dir / "02_fct_cdf.png"):
        generated.append("02_fct_cdf.png")
    else:
        skipped.append("02_fct_cdf.png")

    if plot_fct_by_size_mean(records, out_dir / "03_fct_by_msg_size_cdf.png", args.max_size_groups):
        generated.append("03_fct_by_msg_size_cdf.png")
    else:
        skipped.append("03_fct_by_msg_size_cdf.png")

    if plot_switch_queue(q_pkts_series, q_bytes_series, out_dir / "04_switch_queue_timeseries.png"):
        generated.append("04_switch_queue_timeseries.png")
    else:
        skipped.append("04_switch_queue_timeseries.png")

    if plot_link_throughput(link_series, out_dir / "05_link_throughput_timeseries.png"):
        generated.append("05_link_throughput_timeseries.png")
    else:
        skipped.append("05_link_throughput_timeseries.png")

    stats_path = out_dir / "summary_stats.txt"
    write_stats_file(stats_path, fct_stats, rtt_by_flow_stats, rtt_source)

    print(f"scenario_dir: {scenario_dir}")
    print(f"prefix: {prefix}")
    print(f"output_dir: {out_dir}")
    print(f"rtt_source: {rtt_source or 'none'}")
    print("fct_summary_ms:")
    if math.isnan(fct_stats["mean"]):
        print("  no data")
    else:
        print(
            f"  mean={fct_stats['mean']:.6f} p95={fct_stats['p95']:.6f} p99={fct_stats['p99']:.6f}"
        )
    print("rtt_summary_ms_by_flow:")
    if not rtt_by_flow_stats:
        print("  no data")
    else:
        for flow in sorted(rtt_by_flow_stats):
            s = rtt_by_flow_stats[flow]
            print(f"  {flow} mean={s['mean']:.6f} p95={s['p95']:.6f} p99={s['p99']:.6f}")
    print(f"generated_plots: {generated if generated else 'none'}")
    print(f"skipped_plots: {skipped if skipped else 'none'}")
    print(f"stats_file: {stats_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
