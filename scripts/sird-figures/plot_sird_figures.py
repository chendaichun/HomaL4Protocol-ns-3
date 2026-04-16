#!/usr/bin/env python3
"""Parse Homa/SIRD message traces and generate paper-style comparison plots.

Expected inputs from scratch/HomaL4Protocol-paper-reproduction:
  - MsgTraces_W5_load-XXp_<simIdx>.tr
  - MsgTraces_W5_load-XXp_sird_<simIdx>.tr
  - MsgTraces_W5_load-XXp_sird_<simIdx>.sird-grant.tr (optional)

Generated figures:
  - fig_fct_cdf_homa_vs_sird.png
  - fig_p99_fct_vs_load.png
  - fig_goodput_vs_load.png
  - fig_sird_grant_budget_timeline.png (if grant trace exists)
  - fig_sird_csn_ratio_timeline.png (if grant trace exists)
"""

from __future__ import annotations

import argparse
import math
import re
from collections import defaultdict, deque
from dataclasses import dataclass
from pathlib import Path
from statistics import mean

import matplotlib.pyplot as plt

MSG_TRACE_RE = re.compile(
    r"^([+-])\s+(\d+)\s+(\d+)\s+([0-9.]+:\d+)\s+([0-9.]+:\d+)\s+(\d+)\s*$"
)

SIRD_GRANT_RE = re.compile(
    r"^(\d+)\s+sender=([0-9.]+)\s+txMsgId=(\d+)\s+grantOffset=(\d+)\s+"
    r"senderBudgetPkts=([0-9.+-eE]+)\s+ecnEwma=([0-9.+-eE]+)\s+senderCsn=(\d+)\s*$"
)

FILE_RE = re.compile(r"^MsgTraces_W5_load-(\d+)p(_sird)?_(\d+)\.tr$")


@dataclass
class CompletedMsg:
    mode: str
    load_pct: int
    sim_idx: int
    size_bytes: int
    start_ns: int
    end_ns: int

    @property
    def fct_us(self) -> float:
        return (self.end_ns - self.start_ns) / 1e3


@dataclass
class GrantEvent:
    t_ns: int
    sender: str
    tx_msg_id: int
    grant_offset: int
    sender_budget_pkts: float
    ecn_ewma: float
    sender_csn: int


def find_trace_files(trace_dir: Path) -> list[Path]:
    files = []
    for p in sorted(trace_dir.glob("MsgTraces_W5_load-*.tr")):
        if p.name.endswith(".sird-grant.tr"):
            continue
        m = FILE_RE.match(p.name)
        if m:
            files.append(p)
    return files


def parse_msg_trace(path: Path) -> list[CompletedMsg]:
    m = FILE_RE.match(path.name)
    if not m:
        return []

    load_pct = int(m.group(1))
    mode = "sird" if m.group(2) else "homa"
    sim_idx = int(m.group(3))

    starts: dict[tuple[str, str, int, int], deque[int]] = defaultdict(deque)
    done: list[CompletedMsg] = []

    with path.open("r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            mm = MSG_TRACE_RE.match(line)
            if not mm:
                continue
            sign, t_ns_s, size_s, src, dst, txid_s = mm.groups()
            t_ns = int(t_ns_s)
            size = int(size_s)
            txid = int(txid_s)
            key = (src, dst, txid, size)

            if sign == "+":
                starts[key].append(t_ns)
            else:
                if starts[key]:
                    t0 = starts[key].popleft()
                    if t_ns >= t0:
                        done.append(
                            CompletedMsg(
                                mode=mode,
                                load_pct=load_pct,
                                sim_idx=sim_idx,
                                size_bytes=size,
                                start_ns=t0,
                                end_ns=t_ns,
                            )
                        )
    return done


def parse_sird_grant_trace(path: Path) -> list[GrantEvent]:
    events: list[GrantEvent] = []
    if not path.exists():
        return events
    with path.open("r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            mm = SIRD_GRANT_RE.match(line)
            if not mm:
                continue
            t_ns, sender, tx_msg_id, grant_offset, sender_budget_pkts, ecn_ewma, sender_csn = mm.groups()
            events.append(
                GrantEvent(
                    t_ns=int(t_ns),
                    sender=sender,
                    tx_msg_id=int(tx_msg_id),
                    grant_offset=int(grant_offset),
                    sender_budget_pkts=float(sender_budget_pkts),
                    ecn_ewma=float(ecn_ewma),
                    sender_csn=int(sender_csn),
                )
            )
    return events


def percentile(values: list[float], p: float) -> float:
    if not values:
        return float("nan")
    if len(values) == 1:
        return values[0]
    vals = sorted(values)
    idx = (len(vals) - 1) * p
    lo = math.floor(idx)
    hi = math.ceil(idx)
    if lo == hi:
        return vals[lo]
    w = idx - lo
    return vals[lo] * (1 - w) + vals[hi] * w


def ecdf(values: list[float]) -> tuple[list[float], list[float]]:
    if not values:
        return [], []
    vals = sorted(values)
    n = len(vals)
    x = vals
    y = [(i + 1) / n for i in range(n)]
    return x, y


def compute_goodput_mbps(msgs: list[CompletedMsg]) -> float:
    if not msgs:
        return float("nan")
    total_bits = sum(m.size_bytes for m in msgs) * 8
    t0 = min(m.start_ns for m in msgs)
    t1 = max(m.end_ns for m in msgs)
    if t1 <= t0:
        return float("nan")
    return total_bits / ((t1 - t0) / 1e9) / 1e6


def plot_cdf_by_mode(all_msgs: list[CompletedMsg], out_dir: Path) -> None:
    plt.figure(figsize=(7.5, 5.0))
    for mode, color in [("homa", "#1f77b4"), ("sird", "#d62728")]:
        vals = [m.fct_us for m in all_msgs if m.mode == mode]
        x, y = ecdf(vals)
        if x:
            plt.plot(x, y, label=f"{mode.upper()} (n={len(vals)})", color=color, linewidth=2)
    plt.xlabel("Message Completion Time (us)")
    plt.ylabel("CDF")
    plt.title("FCT CDF: Homa vs Homa+SIRD")
    plt.grid(True, alpha=0.25)
    plt.legend()
    plt.tight_layout()
    plt.savefig(out_dir / "fig_fct_cdf_homa_vs_sird.png", dpi=180)
    plt.close()


def plot_p99_vs_load(all_msgs: list[CompletedMsg], out_dir: Path) -> None:
    grouped: dict[tuple[str, int], list[float]] = defaultdict(list)
    for m in all_msgs:
        grouped[(m.mode, m.load_pct)].append(m.fct_us)

    loads = sorted({m.load_pct for m in all_msgs})
    plt.figure(figsize=(7.5, 5.0))
    for mode, color in [("homa", "#1f77b4"), ("sird", "#d62728")]:
        ys = [percentile(grouped[(mode, l)], 0.99) for l in loads]
        if any(not math.isnan(v) for v in ys):
            plt.plot(loads, ys, marker="o", label=f"{mode.upper()} p99", color=color, linewidth=2)
    plt.xlabel("Offered Load (%)")
    plt.ylabel("p99 FCT (us)")
    plt.title("Tail Latency vs Load")
    plt.grid(True, alpha=0.25)
    plt.legend()
    plt.tight_layout()
    plt.savefig(out_dir / "fig_p99_fct_vs_load.png", dpi=180)
    plt.close()


def plot_goodput_vs_load(all_msgs: list[CompletedMsg], out_dir: Path) -> None:
    per_run: dict[tuple[str, int, int], list[CompletedMsg]] = defaultdict(list)
    for m in all_msgs:
        per_run[(m.mode, m.load_pct, m.sim_idx)].append(m)

    per_mode_load: dict[tuple[str, int], list[float]] = defaultdict(list)
    for key, msgs in per_run.items():
        mode, load, _ = key
        gp = compute_goodput_mbps(msgs)
        if not math.isnan(gp):
            per_mode_load[(mode, load)].append(gp)

    loads = sorted({m.load_pct for m in all_msgs})
    plt.figure(figsize=(7.5, 5.0))
    for mode, color in [("homa", "#1f77b4"), ("sird", "#d62728")]:
        ys = [mean(per_mode_load[(mode, l)]) if per_mode_load[(mode, l)] else float("nan") for l in loads]
        if any(not math.isnan(v) for v in ys):
            plt.plot(loads, ys, marker="o", label=f"{mode.upper()}", color=color, linewidth=2)
    plt.xlabel("Offered Load (%)")
    plt.ylabel("Goodput (Mb/s, app payload)")
    plt.title("Goodput vs Load")
    plt.grid(True, alpha=0.25)
    plt.legend()
    plt.tight_layout()
    plt.savefig(out_dir / "fig_goodput_vs_load.png", dpi=180)
    plt.close()


def plot_sird_grant_timeline(events: list[GrantEvent], out_dir: Path) -> None:
    if not events:
        return

    t0 = events[0].t_ns
    xs_ms = [(e.t_ns - t0) / 1e6 for e in events]

    plt.figure(figsize=(8.5, 5.0))
    ys_budget = [e.sender_budget_pkts for e in events]
    ys_ecn = [e.ecn_ewma for e in events]

    ax1 = plt.gca()
    ax1.plot(xs_ms, ys_budget, color="#2ca02c", linewidth=1.2, label="senderBudgetPkts")
    ax1.set_xlabel("Time since first grant event (ms)")
    ax1.set_ylabel("Sender Budget (pkts)", color="#2ca02c")
    ax1.tick_params(axis="y", labelcolor="#2ca02c")
    ax1.grid(True, alpha=0.2)

    ax2 = ax1.twinx()
    ax2.plot(xs_ms, ys_ecn, color="#9467bd", linewidth=1.2, label="ecnEwma")
    ax2.set_ylabel("ECN EWMA", color="#9467bd")
    ax2.tick_params(axis="y", labelcolor="#9467bd")

    plt.title("SIRD Grant Control Timeline")
    plt.tight_layout()
    plt.savefig(out_dir / "fig_sird_grant_budget_timeline.png", dpi=180)
    plt.close()

    # CSN ratio in fixed windows.
    window_ms = 0.2
    buckets: dict[int, list[int]] = defaultdict(list)
    for x_ms, e in zip(xs_ms, events):
        b = int(x_ms / window_ms)
        buckets[b].append(e.sender_csn)

    bx = sorted(buckets.keys())
    x2 = [b * window_ms for b in bx]
    y2 = [sum(buckets[b]) / max(1, len(buckets[b])) for b in bx]

    plt.figure(figsize=(8.5, 4.6))
    plt.plot(x2, y2, color="#ff7f0e", linewidth=1.8)
    plt.ylim(-0.02, 1.02)
    plt.xlabel("Time since first grant event (ms)")
    plt.ylabel("CSN ratio")
    plt.title("SIRD CSN Ratio Over Time")
    plt.grid(True, alpha=0.25)
    plt.tight_layout()
    plt.savefig(out_dir / "fig_sird_csn_ratio_timeline.png", dpi=180)
    plt.close()


def pick_latest_sird_grant(trace_dir: Path) -> Path | None:
    cands = sorted(trace_dir.glob("MsgTraces_W5_load-*_sird_*.sird-grant.tr"))
    if not cands:
        return None
    cands.sort(key=lambda p: p.stat().st_mtime, reverse=True)
    return cands[0]


def main() -> int:
    parser = argparse.ArgumentParser(description="Plot Homa vs SIRD figures from ns-3 traces")
    parser.add_argument("--trace-dir", default="outputs/homa-paper-reproduction", help="Trace directory")
    parser.add_argument("--out-dir", default="outputs/homa-paper-reproduction/figures-sird", help="Figure output directory")
    parser.add_argument("--grant-trace", default="", help="Optional specific .sird-grant.tr file")
    args = parser.parse_args()

    trace_dir = Path(args.trace_dir).resolve()
    out_dir = Path(args.out_dir).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    trace_files = find_trace_files(trace_dir)
    all_msgs: list[CompletedMsg] = []
    for tf in trace_files:
        all_msgs.extend(parse_msg_trace(tf))

    if not all_msgs:
        print(f"No message traces found in {trace_dir}")
        return 1

    plot_cdf_by_mode(all_msgs, out_dir)
    plot_p99_vs_load(all_msgs, out_dir)
    plot_goodput_vs_load(all_msgs, out_dir)

    grant_trace_path: Path | None
    if args.grant_trace:
        grant_trace_path = Path(args.grant_trace).resolve()
    else:
        grant_trace_path = pick_latest_sird_grant(trace_dir)

    if grant_trace_path and grant_trace_path.exists():
        events = parse_sird_grant_trace(grant_trace_path)
        plot_sird_grant_timeline(events, out_dir)

    print(f"Generated figures in: {out_dir}")
    for p in sorted(out_dir.glob("*.png")):
        print(f" - {p.name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
