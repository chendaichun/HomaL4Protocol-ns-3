#!/usr/bin/env python3
"""Plot lab2 sender-congestion / sender-information traces.

Expected input files are produced by scripts/lab2.sh through
scratch/lab2.cc:
  lab2_<tag>.credit-series.tr
  lab2_<tag>.switch-egress-queue.tr (optional)
  lab2_<tag>.sird-credit.tr (optional)
"""

from __future__ import annotations

import argparse
import csv
from collections import Counter, defaultdict
from pathlib import Path
from typing import DefaultDict, Dict, Iterable, List, Optional, Tuple

import matplotlib.pyplot as plt


Event = Tuple[int, str, int, str]


def parse_kv_fields(tokens: Iterable[str]) -> Dict[str, str]:
    fields: Dict[str, str] = {}
    for token in tokens:
        if "=" not in token:
            continue
        key, value = token.split("=", 1)
        fields[key] = value
    return fields


def parse_credit_events(path: Path) -> List[Event]:
    events: List[Event] = []
    if not path.exists():
        return events

    with path.open("r", encoding="utf-8") as handle:
        for line in handle:
            parts = line.split()
            if not parts:
                continue
            try:
                time_ns = int(parts[0])
            except ValueError:
                continue
            fields = parse_kv_fields(parts[1:])
            if "type" not in fields or "recvNode" not in fields or "sender" not in fields:
                continue
            try:
                recv_node = int(fields["recvNode"])
            except ValueError:
                continue
            events.append((time_ns, fields["type"], recv_node, fields["sender"]))

    events.sort(key=lambda row: row[0])
    return events


def parse_credit_series(path: Path) -> Optional[Tuple[List[float], List[float], List[float], str, List[int]]]:
    if not path.exists():
        return None

    xs: List[float] = []
    sender_accum: List[float] = []
    receiver_avail: List[float] = []
    target_sender = ""
    first_ns: Optional[int] = None

    with path.open("r", encoding="utf-8") as handle:
        for line in handle:
            parts = line.split()
            if not parts:
                continue
            try:
                time_ns = int(parts[0])
            except ValueError:
                continue
            fields = parse_kv_fields(parts[1:])
            if "senderAccumXbdp" not in fields or "receiverAvailXbdp" not in fields:
                continue
            try:
                sender_xbdp = float(fields["senderAccumXbdp"])
                receiver_xbdp = float(fields["receiverAvailXbdp"])
            except ValueError:
                continue
            if first_ns is None:
                first_ns = time_ns
            xs.append((time_ns - first_ns) / 1e9)
            sender_accum.append(sender_xbdp)
            receiver_avail.append(receiver_xbdp)
            target_sender = fields.get("targetSender", target_sender)

    if not xs:
        return None
    return xs, sender_accum, receiver_avail, target_sender, []


def moving_average(values: List[float], window: int) -> List[float]:
    if window <= 1:
        return values[:]
    result: List[float] = []
    running = 0.0
    queue: List[float] = []
    for value in values:
        queue.append(value)
        running += value
        if len(queue) > window:
            running -= queue.pop(0)
        result.append(running / len(queue))
    return result


def derive_credit_series(
    events: List[Event],
    bdp_pkts: int,
    budget_pkts: int,
    dt_s: float,
    ma_s: float,
) -> Optional[Tuple[List[float], List[float], List[float], str, List[int]]]:
    if not events:
        return None

    grants_by_sender: Counter[str] = Counter()
    grants_by_receiver: Counter[int] = Counter()
    for _, event_type, recv_node, sender in events:
        if event_type == "grant":
            grants_by_sender[sender] += 1
            grants_by_receiver[recv_node] += 1

    if not grants_by_sender or not grants_by_receiver:
        return None

    target_sender = grants_by_sender.most_common(1)[0][0]
    receivers = [node for node, _ in grants_by_receiver.most_common(3)]

    outstanding: DefaultDict[Tuple[int, str], int] = defaultdict(int)
    total_by_receiver: DefaultDict[int, int] = defaultdict(int)

    t0 = events[0][0]
    t_end = events[-1][0]
    sample_ns = max(1, int(dt_s * 1e9))
    next_sample = t0
    index = 0

    xs: List[float] = []
    sender_accum_bdp: List[float] = []
    receiver_avail_bdp: List[float] = []

    while next_sample <= t_end:
        while index < len(events) and events[index][0] <= next_sample:
            _, event_type, recv_node, sender = events[index]
            key = (recv_node, sender)
            if event_type == "grant":
                outstanding[key] += 1
                total_by_receiver[recv_node] += 1
            elif event_type == "data":
                if outstanding[key] > 0:
                    outstanding[key] -= 1
                    total_by_receiver[recv_node] = max(0, total_by_receiver[recv_node] - 1)
            index += 1

        sender_accum = sum(outstanding[(recv_node, target_sender)] for recv_node in receivers)
        receiver_avail = sum(
            max(0, budget_pkts - total_by_receiver[recv_node]) for recv_node in receivers
        )

        xs.append((next_sample - t0) / 1e9)
        sender_accum_bdp.append(sender_accum / float(bdp_pkts))
        receiver_avail_bdp.append(receiver_avail / float(bdp_pkts))
        next_sample += sample_ns

    ma_window = max(1, int(round(ma_s / dt_s)))
    return (
        xs,
        moving_average(sender_accum_bdp, ma_window),
        moving_average(receiver_avail_bdp, ma_window),
        target_sender,
        receivers,
    )


def parse_sird_budget(path: Path) -> Dict[str, List[Tuple[float, float, int]]]:
    rows: Dict[str, List[Tuple[float, float, int]]] = defaultdict(list)
    if not path.exists():
        return rows

    first_ns: Optional[int] = None
    with path.open("r", encoding="utf-8") as handle:
        for line in handle:
            parts = line.split()
            if not parts:
                continue
            try:
                time_ns = int(parts[0])
            except ValueError:
                continue
            fields = parse_kv_fields(parts[1:])
            if "sender" not in fields or "senderBudgetPkts" not in fields:
                continue
            try:
                budget = float(fields["senderBudgetPkts"])
                csn = int(fields.get("senderCsn", "0"))
            except ValueError:
                continue
            if first_ns is None:
                first_ns = time_ns
            rows[fields["sender"]].append(((time_ns - first_ns) / 1e9, budget, csn))
    return rows


def parse_switch_sender_queue(path: Path) -> List[Tuple[float, float]]:
    rows: List[Tuple[float, float]] = []
    if not path.exists():
        return rows

    first_ns: Optional[int] = None
    with path.open("r", encoding="utf-8") as handle:
        for line in handle:
            parts = line.split()
            if not parts:
                continue
            try:
                time_ns = int(parts[0])
            except ValueError:
                continue
            fields = parse_kv_fields(parts[1:])
            if fields.get("queue") != "switch_port_to_sender":
                continue
            try:
                packets = float(fields["packets"])
            except (KeyError, ValueError):
                continue
            if first_ns is None:
                first_ns = time_ns
            rows.append(((time_ns - first_ns) / 1e9, packets))
    return rows


def pick_sender(rows: Dict[str, List[Tuple[float, float, int]]]) -> Optional[str]:
    if not rows:
        return None
    return max(rows.keys(), key=lambda sender: len(rows[sender]))


def plot_credit_dynamics(
    out_dir: Path,
    feedback: Tuple[List[float], List[float], List[float], str, List[int]],
    no_feedback: Tuple[List[float], List[float], List[float], str, List[int]],
) -> None:
    fig, axes = plt.subplots(1, 2, figsize=(11.5, 4.7))

    axes[0].plot(feedback[0], feedback[1], label="SIRD feedback", linewidth=2.2)
    axes[0].plot(no_feedback[0], no_feedback[1], label="No sender feedback", linewidth=2.2)
    axes[0].set_xlabel("Time (s)")
    axes[0].set_ylabel("Credit (xBDP)")
    axes[0].set_title("Credit accumulated at sender")
    axes[0].grid(True, alpha=0.3)
    axes[0].legend()

    axes[1].plot(feedback[0], feedback[2], label="SIRD feedback", linewidth=2.2)
    axes[1].plot(no_feedback[0], no_feedback[2], label="No sender feedback", linewidth=2.2)
    axes[1].set_xlabel("Time (s)")
    axes[1].set_ylabel("Credit (xBDP)")
    axes[1].set_title("Total receiver credit available")
    axes[1].grid(True, alpha=0.3)
    axes[1].legend()

    fig.suptitle("Lab2 Sender Congestion / Sender Information", y=1.03)
    fig.tight_layout()
    fig.savefig(out_dir / "lab2_sender_credit_dynamics.png", dpi=180, bbox_inches="tight")
    plt.close(fig)


def plot_sender_budget(
    out_dir: Path,
    feedback_budget: Dict[str, List[Tuple[float, float, int]]],
    no_feedback_budget: Dict[str, List[Tuple[float, float, int]]],
) -> None:
    plt.figure(figsize=(8.5, 4.8))
    plotted = False

    sender = pick_sender(feedback_budget)
    if sender is not None:
        xs = [row[0] for row in feedback_budget[sender]]
        ys = [row[1] for row in feedback_budget[sender]]
        if xs:
            plotted = True
            plt.plot(xs, ys, label=f"SIRD feedback ({sender})", linewidth=1.8)

    sender = pick_sender(no_feedback_budget)
    if sender is not None:
        xs = [row[0] for row in no_feedback_budget[sender]]
        ys = [row[1] for row in no_feedback_budget[sender]]
        if xs:
            plotted = True
            plt.plot(xs, ys, label=f"No sender feedback ({sender})", linewidth=1.8)

    if not plotted:
        plt.close()
        return

    plt.xlabel("Time (s)")
    plt.ylabel("senderBudgetPkts")
    plt.title("Receiver per-sender budget")
    plt.grid(True, alpha=0.3)
    plt.legend()
    plt.tight_layout()
    plt.savefig(out_dir / "lab2_sender_budget_timeseries.png", dpi=180)
    plt.close()


def plot_switch_sender_queue(
    out_dir: Path,
    feedback_rows: List[Tuple[float, float]],
    no_feedback_rows: List[Tuple[float, float]],
) -> None:
    plt.figure(figsize=(8.5, 4.8))
    plotted = False
    if feedback_rows:
        xs, ys = zip(*feedback_rows)
        plt.plot(xs, ys, label="SIRD feedback", linewidth=1.5)
        plotted = True
    if no_feedback_rows:
        xs, ys = zip(*no_feedback_rows)
        plt.plot(xs, ys, label="No sender feedback", linewidth=1.5)
        plotted = True
    if not plotted:
        plt.close()
        return
    plt.xlabel("Time (s)")
    plt.ylabel("Switch egress queue to sender (packets)")
    plt.title("Sender-facing switch egress queue")
    plt.grid(True, alpha=0.3)
    plt.legend()
    plt.tight_layout()
    plt.savefig(out_dir / "lab2_sender_switch_egress_queue.png", dpi=180)
    plt.close()


def write_summary(
    out_dir: Path,
    feedback: Tuple[List[float], List[float], List[float], str, List[int]],
    no_feedback: Tuple[List[float], List[float], List[float], str, List[int]],
) -> None:
    path = out_dir / "lab2_summary.csv"
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(
            [
                "case",
                "target_sender",
                "receiver_nodes",
                "samples",
                "mean_sender_accum_xbdp",
                "final_sender_accum_xbdp",
                "mean_receiver_avail_xbdp",
                "final_receiver_avail_xbdp",
            ]
        )
        for label, series in [("feedback", feedback), ("no_feedback", no_feedback)]:
            xs, sender_accum, receiver_avail, target_sender, receivers = series
            writer.writerow(
                [
                    label,
                    target_sender,
                    " ".join(str(r) for r in receivers),
                    len(xs),
                    sum(sender_accum) / len(sender_accum) if sender_accum else "",
                    sender_accum[-1] if sender_accum else "",
                    sum(receiver_avail) / len(receiver_avail) if receiver_avail else "",
                    receiver_avail[-1] if receiver_avail else "",
                ]
            )


def main() -> int:
    parser = argparse.ArgumentParser(description="Plot lab2 sender-congestion traces.")
    parser.add_argument(
        "--trace-dir",
        type=Path,
        default=Path("outputs/sird-scenarios/HomaL4Protocol-lab2-sender-congestion"),
    )
    parser.add_argument("--out-dir", type=Path, default=Path("output-f/lab2"))
    parser.add_argument("--feedback-tag", default="lab2_feedback")
    parser.add_argument("--no-feedback-tag", default="lab2_no_feedback")
    parser.add_argument("--bdp-pkts", type=int, default=24)
    parser.add_argument("--budget-pkts", type=int, default=36)
    parser.add_argument("--sample-sec", type=float, default=0.01)
    parser.add_argument("--ma-sec", type=float, default=0.1)
    args = parser.parse_args()

    out_dir = args.out_dir
    out_dir.mkdir(parents=True, exist_ok=True)

    feedback = parse_credit_series(args.trace_dir / f"lab2_{args.feedback_tag}.credit-series.tr")
    no_feedback = parse_credit_series(args.trace_dir / f"lab2_{args.no_feedback_tag}.credit-series.tr")

    if feedback is None:
        feedback_events = parse_credit_events(args.trace_dir / f"lab2_{args.feedback_tag}.credit-events.tr")
        feedback = derive_credit_series(
            feedback_events, args.bdp_pkts, args.budget_pkts, args.sample_sec, args.ma_sec
        )
    if no_feedback is None:
        no_feedback_events = parse_credit_events(args.trace_dir / f"lab2_{args.no_feedback_tag}.credit-events.tr")
        no_feedback = derive_credit_series(
            no_feedback_events, args.bdp_pkts, args.budget_pkts, args.sample_sec, args.ma_sec
        )

    if feedback is None or no_feedback is None:
        raise SystemExit("Missing or insufficient lab2 credit-events traces.")

    plot_credit_dynamics(out_dir, feedback, no_feedback)

    feedback_budget = parse_sird_budget(args.trace_dir / f"lab2_{args.feedback_tag}.sird-credit.tr")
    no_feedback_budget = parse_sird_budget(args.trace_dir / f"lab2_{args.no_feedback_tag}.sird-credit.tr")
    plot_sender_budget(out_dir, feedback_budget, no_feedback_budget)
    feedback_queue = parse_switch_sender_queue(args.trace_dir / f"lab2_{args.feedback_tag}.switch-egress-queue.tr")
    no_feedback_queue = parse_switch_sender_queue(args.trace_dir / f"lab2_{args.no_feedback_tag}.switch-egress-queue.tr")
    plot_switch_sender_queue(out_dir, feedback_queue, no_feedback_queue)
    write_summary(out_dir, feedback, no_feedback)

    print(f"Wrote lab2 plots and summary to {out_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
