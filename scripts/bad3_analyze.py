#!/usr/bin/env python3
"""Analyze bad3 SThr sensitivity traces and generate plots + markdown."""

from __future__ import annotations

import argparse
import csv
import os
from collections import defaultdict
from pathlib import Path
from typing import Dict, Iterable, List, Tuple

os.environ.setdefault("MPLBACKEND", "Agg")
os.environ.setdefault("MPLCONFIGDIR", "/tmp/mpl-bad3-cache")

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt


def parse_metadata(path: Path) -> Dict[str, str]:
    data: Dict[str, str] = {}
    with path.open("r", encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if not line or "=" not in line:
                continue
            key, value = line.split("=", 1)
            data[key] = value
    return data


def parse_kv(tokens: Iterable[str]) -> Dict[str, str]:
    fields: Dict[str, str] = {}
    for token in tokens:
        if "=" not in token:
            continue
        key, value = token.split("=", 1)
        fields[key] = value
    return fields


def moving_average(values: List[float], window: int) -> List[float]:
    if window <= 1:
        return values[:]
    acc = 0.0
    buf: List[float] = []
    out: List[float] = []
    for value in values:
        buf.append(value)
        acc += value
        if len(buf) > window:
            acc -= buf.pop(0)
        out.append(acc / len(buf))
    return out


def parse_credit_sample(path: Path, start_sec: float) -> Tuple[List[float], List[float], List[float]]:
    xs: List[float] = []
    sender_credit: List[float] = []
    receiver_avail: List[float] = []
    with path.open("r", encoding="utf-8") as handle:
        for line in handle:
            parts = line.split()
            if not parts:
                continue
            try:
                time_sec = int(parts[0]) / 1e9 - start_sec
            except ValueError:
                continue
            fields = parse_kv(parts[1:])
            if "senderCreditXbdp" not in fields or "receiverAvailXbdp" not in fields:
                continue
            xs.append(time_sec)
            sender_credit.append(float(fields["senderCreditXbdp"]))
            receiver_avail.append(float(fields["receiverAvailXbdp"]))
    return xs, sender_credit, receiver_avail


def parse_goodput(path: Path, start_sec: float) -> Tuple[List[float], List[float]]:
    xs: List[float] = []
    ys: List[float] = []
    with path.open("r", encoding="utf-8") as handle:
        for line in handle:
            parts = line.split()
            if not parts:
                continue
            try:
                time_sec = int(parts[0]) / 1e9 - start_sec
            except ValueError:
                continue
            fields = parse_kv(parts[1:])
            if fields.get("receiver") != "aggregate":
                continue
            if "goodputGbps" not in fields:
                continue
            xs.append(time_sec)
            ys.append(float(fields["goodputGbps"]))
    return xs, ys


def parse_sthr_sample(path: Path, start_sec: float) -> List[Tuple[float, float, int, float]]:
    rows: List[Tuple[float, float, int, float]] = []
    with path.open("r", encoding="utf-8") as handle:
        for line in handle:
            parts = line.split()
            if not parts:
                continue
            try:
                time_sec = int(parts[0]) / 1e9 - start_sec
            except ValueError:
                continue
            fields = parse_kv(parts[1:])
            if "senderBudgetPkts" not in fields:
                continue
            rows.append(
                (
                    time_sec,
                    float(fields["senderBudgetPkts"]),
                    int(fields.get("senderCsn", "0")),
                    float(fields.get("csnRatio", "0")),
                )
            )
    return rows


def bucket_budget_and_csn(
    rows: List[Tuple[float, float, int, float]], window_sec: float
) -> Tuple[List[float], List[float], List[float]]:
    if not rows:
        return [], [], []
    buckets: Dict[int, List[Tuple[float, int, float]]] = defaultdict(list)
    for t_sec, budget, csn, csn_ratio in rows:
        bucket = int(t_sec / window_sec)
        buckets[bucket].append((budget, csn, csn_ratio))

    xs: List[float] = []
    budgets: List[float] = []
    csn_ratios: List[float] = []
    for bucket in sorted(buckets):
        values = buckets[bucket]
        xs.append((bucket + 0.5) * window_sec)
        budgets.append(sum(v[0] for v in values) / len(values))
        csn_ratios.append(sum(v[2] for v in values) / len(values))
    return xs, budgets, csn_ratios


def mean_after(xs: List[float], ys: List[float], cutoff: float) -> float:
    values = [y for x, y in zip(xs, ys) if x >= cutoff]
    return sum(values) / len(values) if values else 0.0


def mean_rows_after(rows: List[Tuple[float, float, int, float]], cutoff: float) -> Tuple[float, float]:
    filtered = [(budget, csn_ratio) for t, budget, _csn, csn_ratio in rows if t >= cutoff]
    if not filtered:
        return 0.0, 0.0
    return (
        sum(v[0] for v in filtered) / len(filtered),
        sum(v[1] for v in filtered) / len(filtered),
    )


def plot_overview(
    out_dir: Path,
    series: Dict[str, Dict[str, object]],
    gap_sec: float,
    duration_sec: float,
) -> None:
    colors = {"low": "#d73027", "mid": "#4575b4", "high": "#f46d43"}
    labels = {
        "low": "SThr=0.1xBDP",
        "mid": "SThr=0.5xBDP",
        "high": "SThr=4.0xBDP",
    }
    stage_marks = [0.0, gap_sec, 2 * gap_sec]

    fig, axes = plt.subplots(3, 2, figsize=(12.5, 10.5), sharex="col")
    axes = axes.flatten()

    for case in ["low", "mid", "high"]:
        color = colors[case]
        label = labels[case]
        payload = series[case]
        axes[0].plot(payload["credit_xs"], payload["sender_credit_ma"], color=color, linewidth=2.0, label=label)
        axes[1].plot(payload["credit_xs"], payload["receiver_avail_ma"], color=color, linewidth=2.0, label=label)
        axes[2].plot(payload["budget_xs"], payload["budget_ma"], color=color, linewidth=2.0, label=label)
        axes[3].plot(payload["budget_xs"], payload["csn_ratio"], color=color, linewidth=2.0, label=label)
        axes[4].plot(payload["goodput_xs"], payload["goodput_ma"], color=color, linewidth=2.0, label=label)

    titles = [
        "Sender-held credit (xBDP)",
        "Receiver-available credit (xBDP)",
        "Receiver senderBudgetPkts",
        "CSN ratio per 100ms window",
        "Aggregate receiver goodput (Gbps)",
    ]
    ylabels = ["xBDP", "xBDP", "Packets", "Ratio", "Gbps"]

    for idx in range(5):
        ax = axes[idx]
        ax.set_title(titles[idx])
        ax.set_ylabel(ylabels[idx])
        ax.grid(True, alpha=0.25)
        for mark in stage_marks:
            ax.axvline(mark, color="#666666", linewidth=0.9, linestyle="--", alpha=0.55)
        ax.set_xlim(0.0, max(duration_sec - 0.2, 0.1))

    axes[4].set_xlabel("Time since first receiver start (s)")
    axes[5].axis("off")
    handles, leg_labels = axes[0].get_legend_handles_labels()
    axes[5].legend(handles, leg_labels, loc="center", frameon=False, fontsize=11)
    axes[5].text(
        0.04,
        0.28,
        "Dashed lines: R1/R2/R3 start times\n"
        "low: frequent CSN, lower senderBudget\n"
        "high: almost no CSN, credit stays at sender",
        fontsize=10.5,
        va="top",
    )

    fig.suptitle("bad3: SThr Sensitivity under Time-Varying Sender Bottleneck", y=0.995)
    fig.tight_layout()
    fig.savefig(out_dir / "bad3_sthr_sensitivity_overview.png", dpi=220, bbox_inches="tight")
    plt.close(fig)


def write_summary(path: Path, rows: List[Dict[str, object]]) -> None:
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)


def write_md(
    path: Path,
    summary_rows: List[Dict[str, object]],
    metadata: Dict[str, str],
) -> None:
    by_case = {row["case"]: row for row in summary_rows}
    with path.open("w", encoding="utf-8") as handle:
        handle.write("# bad3: SThr 敏感性实验\n\n")
        handle.write("## 场景\n\n")
        handle.write(
            "bad3 复用单 sender、三 receiver 的 sender-bottleneck 拓扑，但把关注点从“有没有 sender feedback”改为“SThr 取值是否稳健”。"
            "三个 receiver 按固定间隔依次加入，因此发送端瓶颈强度会随时间变化。我们比较三档阈值：\n\n"
        )
        handle.write(
            f"- `low`: `SThr = {metadata['lowSthrFactor']} x BDP`（{metadata['lowSthrPkts']} packets）\n"
            f"- `mid`: `SThr = {metadata['midSthrFactor']} x BDP`（{metadata['midSthrPkts']} packets）\n"
            f"- `high`: `SThr = {metadata['highSthrFactor']} x BDP`（{metadata['highSthrPkts']} packets）\n\n"
        )
        handle.write("![bad3 overview](plots/bad3_sthr_sensitivity_overview.png)\n\n")
        handle.write("## 关键数值（R3 加入后的稳态区间）\n\n")
        handle.write("| Case | mean sender credit (xBDP) | mean receiver avail (xBDP) | mean senderBudgetPkts | CSN ratio | mean goodput (Gbps) |\n")
        handle.write("|---|---:|---:|---:|---:|---:|\n")
        for case in ["low", "mid", "high"]:
            row = by_case[case]
            handle.write(
                f"| {case} | {row['mean_sender_credit_xbdp']:.3f} | {row['mean_receiver_avail_xbdp']:.3f} | "
                f"{row['mean_sender_budget_pkts']:.2f} | {row['mean_csn_ratio']:.3f} | {row['mean_goodput_gbps']:.3f} |\n"
            )
        handle.write("\n## 结论\n\n")
        handle.write(
            "1. `SThr` 过低时，`CSN ratio` 明显升高，receiver 侧 `senderBudgetPkts` 被压得更低，"
            "说明阈值过于敏感，会把正常的 sender 瓶颈也当成需要强烈抑制的拥塞信号。\n\n"
        )
        handle.write(
            "2. `SThr` 适中时，credit 不会像高阈值那样长期滞留在 sender，"
            "也不会像低阈值那样持续压低 budget，因此 aggregate goodput 最接近平衡点。\n\n"
        )
        handle.write(
            "3. `SThr` 过高时，`CSN ratio` 接近 0，sender-held credit 明显更高，receiver-available credit 更低，"
            "说明 sender 已经成为瓶颈，但 receiver 仍继续把 credit 发给该 sender，造成 credit 长期占用和浪费。\n\n"
        )
        handle.write(
            "因此，`SThr` 并不是一个对所有网络环境都稳健的固定阈值。"
            "当 sender bottleneck 强度随时间或场景变化时，过低的阈值会过度抑制发送，过高的阈值又无法及时回收 credit。"
        )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--out-dir", type=Path, required=True)
    args = parser.parse_args()

    metadata = parse_metadata(args.root / "metadata.txt")
    start_sec = float(metadata["startSec"])
    duration_sec = float(metadata["durationSec"])
    gap_sec = float(metadata["flowGapUs"]) / 1e6
    stage3_start = 2 * gap_sec
    stage3_tail = max(duration_sec - stage3_start, 0.0)
    stage3_cutoff = stage3_start + min(0.2, stage3_tail * 0.25)
    ma_window = 8

    args.out_dir.mkdir(parents=True, exist_ok=True)

    series: Dict[str, Dict[str, object]] = {}
    summary_rows: List[Dict[str, object]] = []

    for case in ["low", "mid", "high"]:
        credit_xs, sender_credit, receiver_avail = parse_credit_sample(args.root / f"bad3_{case}.credit-sample.tr", start_sec)
        goodput_xs, goodput = parse_goodput(args.root / f"bad3_{case}.goodput.tr", start_sec)
        budget_rows = parse_sthr_sample(args.root / f"bad3_{case}.sthr-sample.tr", start_sec)
        budget_xs, budget_ma, csn_ratio = bucket_budget_and_csn(budget_rows, 0.1)

        payload = {
            "credit_xs": credit_xs,
            "sender_credit_ma": moving_average(sender_credit, ma_window),
            "receiver_avail_ma": moving_average(receiver_avail, ma_window),
            "goodput_xs": goodput_xs,
            "goodput_ma": moving_average(goodput, ma_window),
            "budget_xs": budget_xs,
            "budget_ma": budget_ma,
            "csn_ratio": csn_ratio,
        }
        series[case] = payload

        mean_budget, mean_csn = mean_rows_after(budget_rows, stage3_cutoff)
        summary_rows.append(
            {
                "case": case,
                "mean_sender_credit_xbdp": mean_after(credit_xs, sender_credit, stage3_cutoff),
                "mean_receiver_avail_xbdp": mean_after(credit_xs, receiver_avail, stage3_cutoff),
                "mean_sender_budget_pkts": mean_budget,
                "mean_csn_ratio": mean_csn,
                "mean_goodput_gbps": mean_after(goodput_xs, goodput, stage3_cutoff),
            }
        )

    plot_overview(args.out_dir, series, gap_sec, duration_sec)
    write_summary(args.out_dir / "bad3_summary.csv", summary_rows)
    write_md(args.root / "bad3_analysis.md", summary_rows, metadata)
    print(f"Wrote bad3 plots and summary to {args.out_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
