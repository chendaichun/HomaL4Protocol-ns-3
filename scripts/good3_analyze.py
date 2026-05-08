#!/usr/bin/env python3
"""Analyze good3 core-congestion traces and generate plots + markdown."""

from __future__ import annotations

import argparse
import csv
from pathlib import Path
from typing import Dict, Iterable, List, Tuple

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt


def parse_kv(tokens: Iterable[str]) -> Dict[str, str]:
    values: Dict[str, str] = {}
    for token in tokens:
        if "=" not in token:
            continue
        key, value = token.split("=", 1)
        values[key] = value
    return values


def parse_goodput(path: Path, start_sec: float) -> Tuple[List[float], List[float]]:
    xs: List[float] = []
    ys: List[float] = []
    if not path.exists():
      return xs, ys
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
            if "goodputGbps" not in fields:
                continue
            xs.append(time_sec)
            ys.append(float(fields["goodputGbps"]))
    return xs, ys


def parse_queue(path: Path, start_sec: float) -> Tuple[List[float], List[float], List[float]]:
    xs: List[float] = []
    max_pkts: List[float] = []
    mean_pkts: List[float] = []
    if not path.exists():
        return xs, max_pkts, mean_pkts
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
            if fields.get("queue") != "aggregate":
                continue
            try:
                max_pkts.append(float(fields["maxPackets"]))
                mean_pkts.append(float(fields["meanPackets"]))
            except (KeyError, ValueError):
                continue
            xs.append(time_sec)
    return xs, max_pkts, mean_pkts


def moving_average(values: List[float], window: int) -> List[float]:
    if window <= 1:
        return values[:]
    out: List[float] = []
    acc = 0.0
    buf: List[float] = []
    for value in values:
        buf.append(value)
        acc += value
        if len(buf) > window:
            acc -= buf.pop(0)
        out.append(acc / len(buf))
    return out


def mean_after(xs: List[float], ys: List[float], cutoff: float) -> float:
    samples = [y for x, y in zip(xs, ys) if x >= cutoff]
    return sum(samples) / len(samples) if samples else 0.0


def p95_after(xs: List[float], ys: List[float], cutoff: float) -> float:
    samples = sorted(y for x, y in zip(xs, ys) if x >= cutoff)
    if not samples:
        return 0.0
    idx = min(len(samples) - 1, int(round(0.95 * (len(samples) - 1))))
    return samples[idx]


def plot_series(
    out_dir: Path,
    payloads: Dict[str, Dict[str, List[float]]],
) -> None:
    colors = {"control": "#1f77b4", "no_ecn": "#d62728"}
    labels = {"control": "ECN control", "no_ecn": "No ECN control"}
    fig, axes = plt.subplots(2, 1, figsize=(10.5, 7.0), sharex=True)
    for case in ("control", "no_ecn"):
        payload = payloads[case]
        axes[0].plot(
            payload["goodput_xs"],
            moving_average(payload["goodput"], 10),
            linewidth=2.0,
            color=colors[case],
            label=labels[case],
        )
        axes[1].plot(
            payload["queue_xs"],
            moving_average(payload["queue_max"], 10),
            linewidth=2.0,
            color=colors[case],
            label=labels[case],
        )

    axes[0].set_ylabel("Aggregate goodput (Gbps)")
    axes[0].set_title("good3: core congestion aggregate goodput")
    axes[0].grid(True, alpha=0.25)
    axes[0].legend(frameon=False)

    axes[1].set_ylabel("Aggregate max queue (pkts)")
    axes[1].set_xlabel("Time since traffic start (s)")
    axes[1].set_title("good3: core congestion queue occupancy")
    axes[1].grid(True, alpha=0.25)

    fig.tight_layout()
    fig.savefig(out_dir / "good3_core_congestion_overview.png", dpi=220, bbox_inches="tight")
    plt.close(fig)


def write_summary(path: Path, rows: List[Dict[str, float | str]]) -> None:
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(
            handle,
            fieldnames=[
                "case",
                "mean_goodput_gbps",
                "mean_queue_max_pkts",
                "p95_queue_max_pkts",
                "mean_queue_mean_pkts",
            ],
        )
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


def write_md(path: Path, summary_rows: List[Dict[str, float | str]]) -> None:
    by_case = {str(row["case"]): row for row in summary_rows}
    control = by_case["control"]
    no_ecn = by_case["no_ecn"]
    with path.open("w", encoding="utf-8") as handle:
        handle.write("# good3：核心链路过载场景\n\n")
        handle.write("## 场景\n\n")
        handle.write(
            "`good3` 构造 4 个发送端、4 个接收端和两台交换机组成的紧凑 dumbbell 拓扑，"
            "让两台交换机之间的共享核心链路成为主要拥塞点。对照采用方案 B：\n\n"
            "- `control`：保留 ECN 标记与 receiver-side ECN AIMD；\n"
            "- `no_ecn`：把 ECN 标记阈值抬高到近乎不触发，并关闭 ECN AIMD。\n\n"
        )
        handle.write("![good3 overview](good3_core_congestion_overview.png)\n\n")
        handle.write("## 关键数值\n\n")
        handle.write("| Case | mean goodput (Gbps) | mean max queue (pkts) | p95 max queue (pkts) | mean queue (pkts) |\n")
        handle.write("|---|---:|---:|---:|---:|\n")
        for case in ("control", "no_ecn"):
            row = by_case[case]
            handle.write(
                f"| {case} | {float(row['mean_goodput_gbps']):.3f} | "
                f"{float(row['mean_queue_max_pkts']):.3f} | "
                f"{float(row['p95_queue_max_pkts']):.3f} | "
                f"{float(row['mean_queue_mean_pkts']):.3f} |\n"
            )
        handle.write("\n## 图表达什么\n\n")
        handle.write(
            "上图上半部分比较 aggregate goodput，下半部分比较 core-facing 队列占用。"
            "如果 `control` 能在保持接近的吞吐下显著压低队列，则说明 ECN/AIMD 确实限制了核心链路积压。\n\n"
        )
        handle.write("## 可以据此写出的结论\n\n")
        handle.write(
            f"1. `control` 的平均 goodput 为 `{float(control['mean_goodput_gbps']):.3f}Gbps`，"
            f"`no_ecn` 为 `{float(no_ecn['mean_goodput_gbps']):.3f}Gbps`。该对比用于判断 ECN 控制是否以明显吞吐损失为代价。\n"
        )
        handle.write(
            f"2. `control` 的 `p95 max queue` 为 `{float(control['p95_queue_max_pkts']):.3f}` packets，"
            f"`no_ecn` 为 `{float(no_ecn['p95_queue_max_pkts']):.3f}` packets。若前者明显更低，说明核心排队被有效限制。\n"
        )
        handle.write(
            "3. 因此，这个场景主要验证的是：SIRD 利用 ECN 作为 core-side congestion signal，"
            "通过接收端 AIMD 收缩 credit 上限，从而抑制共享核心链路上的排队积压。\n"
        )


def main() -> int:
    parser = argparse.ArgumentParser(description="Analyze good3 core-congestion traces.")
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--out-dir", type=Path, required=True)
    parser.add_argument("--start-sec", type=float, default=0.2)
    args = parser.parse_args()

    args.out_dir.mkdir(parents=True, exist_ok=True)
    payloads: Dict[str, Dict[str, List[float]]] = {}
    rows: List[Dict[str, float | str]] = []
    for case in ("control", "no_ecn"):
        goodput_xs, goodput = parse_goodput(args.root / f"sim1_{case}.goodput.tr", args.start_sec)
        queue_xs, queue_max, queue_mean = parse_queue(args.root / f"sim1_{case}.tor-egress-queue.tr", args.start_sec)
        payloads[case] = {
            "goodput_xs": goodput_xs,
            "goodput": goodput,
            "queue_xs": queue_xs,
            "queue_max": queue_max,
            "queue_mean": queue_mean,
        }
        cutoff = 0.02
        rows.append(
            {
                "case": case,
                "mean_goodput_gbps": mean_after(goodput_xs, goodput, cutoff),
                "mean_queue_max_pkts": mean_after(queue_xs, queue_max, cutoff),
                "p95_queue_max_pkts": p95_after(queue_xs, queue_max, cutoff),
                "mean_queue_mean_pkts": mean_after(queue_xs, queue_mean, cutoff),
            }
        )

    plot_series(args.out_dir, payloads)
    write_summary(args.out_dir / "good3_summary.csv", rows)
    write_md(args.out_dir / "good3_report_zh.md", rows)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
