#!/usr/bin/env python3
"""Generate a paper-style Figure 4 plot for the lab2 sender-feedback scene."""

from __future__ import annotations

import argparse
import csv
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, List, Tuple

import matplotlib.pyplot as plt


@dataclass
class Series:
    time_s: List[float]
    sender_credit_xbdp: List[float]
    receiver_avail_xbdp: List[float]


def parse_kv(tokens: Iterable[str]) -> Dict[str, str]:
    fields: Dict[str, str] = {}
    for token in tokens:
        if "=" not in token:
            continue
        key, value = token.split("=", 1)
        fields[key] = value
    return fields


def load_credit_sample(path: Path, start_sec: float) -> Series:
    time_s: List[float] = []
    sender_credit_xbdp: List[float] = []
    receiver_avail_xbdp: List[float] = []

    with path.open("r", encoding="utf-8") as handle:
        for line in handle:
            parts = line.split()
            if not parts:
                continue
            try:
                time_ns = int(parts[0])
            except ValueError:
                continue
            fields = parse_kv(parts[1:])
            if "senderCreditXbdp" not in fields or "receiverAvailXbdp" not in fields:
                continue
            t = time_ns / 1e9
            if t < start_sec:
                continue
            time_s.append(t - start_sec)
            sender_credit_xbdp.append(float(fields["senderCreditXbdp"]))
            receiver_avail_xbdp.append(float(fields["receiverAvailXbdp"]))

    return Series(time_s, sender_credit_xbdp, receiver_avail_xbdp)


def moving_average(values: List[float], window: int) -> List[float]:
    if window <= 1:
        return values[:]
    result: List[float] = []
    queue: List[float] = []
    running = 0.0
    for value in values:
        queue.append(value)
        running += value
        if len(queue) > window:
            running -= queue.pop(0)
        result.append(running / len(queue))
    return result


def smooth(series: Series, sample_us: float, ma_sec: float) -> Series:
    window = max(1, int(round(ma_sec / (sample_us / 1e6))))
    return Series(
        series.time_s,
        moving_average(series.sender_credit_xbdp, window),
        moving_average(series.receiver_avail_xbdp, window),
    )


def percentile(values: List[float], pct: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    index = min(len(ordered) - 1, max(0, int(round((pct / 100.0) * (len(ordered) - 1)))))
    return ordered[index]


def summarize_case(name: str, series: Series, stage_starts: List[float]) -> Dict[str, float | str]:
    last_start = stage_starts[-1] if stage_starts else 0.0
    post_last = [
        (sender, receiver)
        for t, sender, receiver in zip(
            series.time_s, series.sender_credit_xbdp, series.receiver_avail_xbdp
        )
        if t >= last_start
    ]
    sender_values = [row[0] for row in post_last] or series.sender_credit_xbdp
    receiver_values = [row[1] for row in post_last] or series.receiver_avail_xbdp
    return {
        "case": name,
        "samples": len(series.time_s),
        "sender_mean_xbdp": sum(series.sender_credit_xbdp) / len(series.sender_credit_xbdp),
        "sender_final_xbdp": series.sender_credit_xbdp[-1],
        "sender_post_last_mean_xbdp": sum(sender_values) / len(sender_values),
        "sender_post_last_p95_xbdp": percentile(sender_values, 95.0),
        "receiver_mean_xbdp": sum(series.receiver_avail_xbdp) / len(series.receiver_avail_xbdp),
        "receiver_final_xbdp": series.receiver_avail_xbdp[-1],
        "receiver_post_last_mean_xbdp": sum(receiver_values) / len(receiver_values),
        "receiver_post_last_p05_xbdp": percentile(receiver_values, 5.0),
    }


def write_summary(path: Path, rows: List[Dict[str, float | str]]) -> None:
    fieldnames = [
        "case",
        "samples",
        "sender_mean_xbdp",
        "sender_final_xbdp",
        "sender_post_last_mean_xbdp",
        "sender_post_last_p95_xbdp",
        "receiver_mean_xbdp",
        "receiver_final_xbdp",
        "receiver_post_last_mean_xbdp",
        "receiver_post_last_p05_xbdp",
    ]
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


def plot_figure(
    feedback: Series,
    no_feedback: Series,
    out_path: Path,
    stage_starts: List[float],
    annotated: bool,
) -> None:
    plt.rcParams.update(
        {
            "font.size": 9,
            "axes.labelsize": 9,
            "axes.titlesize": 9,
            "legend.fontsize": 8,
            "xtick.labelsize": 8,
            "ytick.labelsize": 8,
            "axes.spines.top": False,
            "axes.spines.right": False,
            "pdf.fonttype": 42,
            "ps.fonttype": 42,
        }
    )

    fig_height = 2.65 if annotated else 2.35
    fig, axes = plt.subplots(1, 2, figsize=(7.0, fig_height), sharex=True)
    colors = {
        "feedback": "#1f77b4",
        "no_feedback": "#d62728",
    }
    labels = {
        "feedback": r"$S_{Thr}=0.5\times BDP$",
        "no_feedback": r"$S_{Thr}=\infty$",
    }

    axes[0].plot(
        feedback.time_s,
        feedback.sender_credit_xbdp,
        color=colors["feedback"],
        linewidth=1.6,
        label=labels["feedback"],
    )
    axes[0].plot(
        no_feedback.time_s,
        no_feedback.sender_credit_xbdp,
        color=colors["no_feedback"],
        linewidth=1.6,
        label=labels["no_feedback"],
    )
    axes[0].set_title("(a) Credit accumulated at sender")
    axes[0].set_ylabel("Credit (BDP)")
    axes[0].set_ylim(0.0, 2.25)

    axes[1].plot(
        feedback.time_s,
        feedback.receiver_avail_xbdp,
        color=colors["feedback"],
        linewidth=1.6,
        label=labels["feedback"],
    )
    axes[1].plot(
        no_feedback.time_s,
        no_feedback.receiver_avail_xbdp,
        color=colors["no_feedback"],
        linewidth=1.6,
        label=labels["no_feedback"],
    )
    axes[1].set_title("(b) Credit available at receivers")
    axes[1].set_ylim(1.2, 4.7)

    for axis in axes:
        axis.set_xlabel("Time since first receiver starts (s)")
        axis.set_xlim(-0.6, 13.2)
        axis.set_xticks([0, 3, 6, 9, 12])
        axis.grid(True, color="#dddddd", linewidth=0.6, alpha=0.8)
        if annotated:
            y_min, y_max = axis.get_ylim()
            label_y = y_min + 0.08 * (y_max - y_min)
            for idx, start in enumerate(stage_starts):
                axis.axvline(start, color="#777777", linestyle="--", linewidth=0.8, alpha=0.55)
                axis.text(
                    start + 0.08,
                    label_y,
                    f"R{idx + 1}",
                    color="#555555",
                    fontsize=7,
                    va="bottom",
                    ha="left",
                    bbox={
                        "facecolor": "white",
                        "edgecolor": "none",
                        "alpha": 0.75,
                        "pad": 0.4,
                    },
                )

    handles, legend_labels = axes[0].get_legend_handles_labels()
    if annotated:
        fig.legend(
            handles,
            legend_labels,
            loc="lower center",
            bbox_to_anchor=(0.5, -0.01),
            ncol=2,
            frameon=False,
        )
        fig.tight_layout(w_pad=1.2, rect=(0.0, 0.12, 1.0, 1.0))
    else:
        axes[0].legend(loc="upper left", frameon=False)
        fig.tight_layout(w_pad=1.2)
    fig.savefig(out_path, dpi=300, bbox_inches="tight")
    fig.savefig(out_path.with_suffix(".pdf"), bbox_inches="tight")
    plt.close(fig)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--raw-dir", type=Path, required=True)
    parser.add_argument("--out-dir", type=Path, required=True)
    parser.add_argument("--start-sec", type=float, default=0.2)
    parser.add_argument("--flow-gap-sec", type=float, default=4.5)
    parser.add_argument("--sample-us", type=float, default=100.0)
    parser.add_argument("--ma-sec", type=float, default=0.1)
    args = parser.parse_args()

    args.out_dir.mkdir(parents=True, exist_ok=True)
    feedback_raw = load_credit_sample(args.raw_dir / "lab2_feedback.credit-sample.tr", args.start_sec)
    no_feedback_raw = load_credit_sample(
        args.raw_dir / "lab2_no_feedback.credit-sample.tr", args.start_sec
    )
    feedback = smooth(feedback_raw, args.sample_us, args.ma_sec)
    no_feedback = smooth(no_feedback_raw, args.sample_us, args.ma_sec)

    stage_starts = [0.0, args.flow_gap_sec, args.flow_gap_sec * 2.0]
    plot_figure(
        feedback,
        no_feedback,
        args.out_dir / "lab2_figure4_paper_style.png",
        stage_starts,
        annotated=False,
    )
    plot_figure(
        feedback,
        no_feedback,
        args.out_dir / "lab2_figure4_with_receiver_starts.png",
        stage_starts,
        annotated=True,
    )

    write_summary(
        args.out_dir / "lab2_figure4_summary.csv",
        [
            summarize_case("feedback_sthr_0p5bdp", feedback, stage_starts),
            summarize_case("no_feedback_sthr_inf", no_feedback, stage_starts),
        ],
    )


if __name__ == "__main__":
    main()
