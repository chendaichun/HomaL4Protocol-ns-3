#!/usr/bin/env python3
"""Create paper-style Figure 3 plots for the lab1_o final run."""

from __future__ import annotations

import argparse
import csv
import math
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Tuple

import matplotlib.pyplot as plt


TRACE_FILES = {
    "unloaded_8b": ("lab1_lab1_fast_sird_unloaded_8B.msg.tr", 8, "Unloaded"),
    "incast_8b": ("lab1_lab1_fast_sird_incast_8B_srpt.msg.tr", 8, "Incast"),
    "unloaded_500kb": ("lab1_lab1_fast_sird_unloaded_500KB.msg.tr", 500000, "Unloaded"),
    "incast_500kb_srpt": (
        "lab1_lab1_fast_sird_incast_500KB_srpt.msg.tr",
        500000,
        "Incast-SRPT",
    ),
    "incast_500kb_srr": (
        "lab1_lab1_fast_sird_incast_500KB_srr.msg.tr",
        500000,
        "Incast-SRR",
    ),
}


COLORS = {
    "Unloaded": "#d99a00",
    "Incast": "#ff3b3b",
    "Incast-SRPT": "#208a20",
    "Incast-SRR": "#d65f00",
}

LINESTYLES = {
    "Unloaded": (0, (5, 2)),
    "Incast": "solid",
    "Incast-SRPT": "solid",
    "Incast-SRR": "solid",
}


def parse_latencies_us(path: Path, expected_size: int) -> List[float]:
    starts: Dict[Tuple[str, str, str, int], int] = {}
    latencies: List[float] = []

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
            if size != expected_size:
                continue

            key = (src, dst, tx_msg_id, size)
            if event == "+":
                starts[key] = time_ns
            elif event == "-":
                start_ns = starts.pop(key, None)
                if start_ns is not None and time_ns >= start_ns:
                    latencies.append((time_ns - start_ns) / 1000.0)

    latencies.sort()
    return latencies


def cdf_points(values: Iterable[float]) -> Tuple[List[float], List[float]]:
    xs = sorted(v for v in values if math.isfinite(v))
    if not xs:
        return [], []
    n = len(xs)
    ys = [(idx + 1) / n for idx in range(n)]
    return xs, ys


def percentile(values: List[float], pct: float) -> Optional[float]:
    if not values:
        return None
    xs = sorted(values)
    if len(xs) == 1:
        return xs[0]
    rank = (len(xs) - 1) * pct / 100.0
    low = int(math.floor(rank))
    high = int(math.ceil(rank))
    if low == high:
        return xs[low]
    frac = rank - low
    return xs[low] * (1.0 - frac) + xs[high] * frac


def load_all(trace_dir: Path) -> Dict[str, List[float]]:
    data: Dict[str, List[float]] = {}
    missing = []
    for key, (name, size, _) in TRACE_FILES.items():
        path = trace_dir / name
        if not path.exists():
            missing.append(str(path))
            continue
        data[key] = parse_latencies_us(path, size)
    if missing:
        raise SystemExit("Missing required traces:\n" + "\n".join(missing))
    return data


def style_axis(ax: plt.Axes, xlim: Tuple[float, float], xticks: List[float]) -> None:
    ax.set_xlim(*xlim)
    ax.set_ylim(-0.05, 1.05)
    ax.set_xticks(xticks)
    ax.set_yticks([0.0, 0.5, 1.0])
    ax.grid(True, which="major", color="#a8a8a8", linewidth=0.9)
    for spine in ax.spines.values():
        spine.set_linewidth(0.9)
        spine.set_color("black")
    ax.tick_params(labelsize=9, width=0.9, length=4)
    ax.set_xlabel("Latency (us)", fontsize=10)


def plot_one_axis(
    ax: plt.Axes,
    series: List[Tuple[str, List[float]]],
    xlim: Tuple[float, float],
    xticks: List[float],
    legend_loc: str,
) -> None:
    for label, values in series:
        xs, ys = cdf_points(values)
        ax.plot(
            xs,
            ys,
            label=label,
            color=COLORS[label],
            linewidth=2.0,
            linestyle=LINESTYLES.get(label, "solid"),
        )
    style_axis(ax, xlim, xticks)
    ax.legend(loc=legend_loc, frameon=False, fontsize=9, handlelength=2.4)


def plot_zoomed_8b(ax: plt.Axes, unloaded: List[float], incast: List[float]) -> None:
    series = [("Unloaded", unloaded), ("Incast", incast)]
    for label, values in series:
        xs, ys = cdf_points(values)
        ax.plot(
            xs,
            ys,
            label=label,
            color=COLORS[label],
            linewidth=2.2,
            linestyle=LINESTYLES.get(label, "solid"),
        )

    style_axis(ax, (17.98, 18.22), [18.0, 18.1, 18.2])
    ax.set_ylabel("CDF", fontsize=10)

    unloaded_p50 = percentile(unloaded, 50) or 0.0
    incast_p50 = percentile(incast, 50) or 0.0
    ax.scatter(
        [unloaded_p50, incast_p50],
        [0.5, 0.5],
        color=[COLORS["Unloaded"], COLORS["Incast"]],
        s=24,
        zorder=3,
    )
    ax.annotate(
        f"Unloaded p50={unloaded_p50:.3f}us",
        xy=(unloaded_p50, 0.5),
        xytext=(18.03, 0.32),
        fontsize=8.5,
        color=COLORS["Unloaded"],
        arrowprops={"arrowstyle": "-", "lw": 0.8, "color": COLORS["Unloaded"]},
    )
    ax.annotate(
        f"Incast p50={incast_p50:.3f}us",
        xy=(incast_p50, 0.5),
        xytext=(18.115, 0.68),
        fontsize=8.5,
        color=COLORS["Incast"],
        arrowprops={"arrowstyle": "-", "lw": 0.8, "color": COLORS["Incast"]},
    )
    ax.legend(loc="lower right", frameon=False, fontsize=9, handlelength=2.4)


def write_summary(out_dir: Path, data: Dict[str, List[float]]) -> None:
    rows = []
    for key, (_, size, label) in TRACE_FILES.items():
        values = data[key]
        rows.append(
            {
                "case": key,
                "label": label,
                "size_bytes": size,
                "count": len(values),
                "p50_us": f"{percentile(values, 50) or 0:.6f}",
                "p90_us": f"{percentile(values, 90) or 0:.6f}",
                "p99_us": f"{percentile(values, 99) or 0:.6f}",
                "mean_us": f"{(sum(values) / len(values)) if values else 0:.6f}",
            }
        )

    with (out_dir / "lab1_o_fig3_paper_summary.csv").open(
        "w", encoding="utf-8", newline=""
    ) as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--trace-dir", type=Path, required=True)
    parser.add_argument("--out-dir", type=Path, required=True)
    args = parser.parse_args()

    args.out_dir.mkdir(parents=True, exist_ok=True)
    data = load_all(args.trace_dir)

    plt.rcParams.update(
        {
            "font.family": "DejaVu Sans",
            "axes.linewidth": 0.9,
            "pdf.fonttype": 42,
            "ps.fonttype": 42,
        }
    )

    fig, axes = plt.subplots(1, 2, figsize=(6.2, 2.45), sharey=True)
    plot_one_axis(
        axes[0],
        [
            ("Unloaded", data["unloaded_8b"]),
            ("Incast", data["incast_8b"]),
        ],
        (0, 50),
        [0, 20, 40],
        "lower right",
    )
    axes[0].set_ylabel("CDF", fontsize=10)

    plot_one_axis(
        axes[1],
        [
            ("Unloaded", data["unloaded_500kb"]),
            ("Incast-SRPT", data["incast_500kb_srpt"]),
            ("Incast-SRR", data["incast_500kb_srr"]),
        ],
        (0, 800),
        [0, 200, 400, 600, 800],
        "lower right",
    )

    fig.subplots_adjust(left=0.08, right=0.995, bottom=0.22, top=0.97, wspace=0.25)
    combined_png = args.out_dir / "lab1_o_fig3_paper_style.png"
    combined_pdf = args.out_dir / "lab1_o_fig3_paper_style.pdf"
    fig.savefig(combined_png, dpi=300, bbox_inches="tight")
    fig.savefig(combined_pdf, bbox_inches="tight")
    plt.close(fig)

    for name, series, xlim, xticks in [
        (
            "lab1_o_fig3_8B_cdf.png",
            [("Unloaded", data["unloaded_8b"]), ("Incast", data["incast_8b"])],
            (0, 50),
            [0, 20, 40],
        ),
        (
            "lab1_o_fig3_500KB_cdf.png",
            [
                ("Unloaded", data["unloaded_500kb"]),
                ("Incast-SRPT", data["incast_500kb_srpt"]),
                ("Incast-SRR", data["incast_500kb_srr"]),
            ],
            (0, 800),
            [0, 200, 400, 600, 800],
        ),
    ]:
        single_fig, ax = plt.subplots(figsize=(3.1, 2.45))
        plot_one_axis(ax, series, xlim, xticks, "lower right")
        ax.set_ylabel("CDF", fontsize=10)
        single_fig.subplots_adjust(left=0.16, right=0.99, bottom=0.22, top=0.97)
        single_fig.savefig(args.out_dir / name, dpi=300, bbox_inches="tight")
        plt.close(single_fig)

    zoom_fig, zoom_ax = plt.subplots(figsize=(3.1, 2.45))
    plot_zoomed_8b(zoom_ax, data["unloaded_8b"], data["incast_8b"])
    zoom_fig.subplots_adjust(left=0.18, right=0.99, bottom=0.22, top=0.97)
    zoom_fig.savefig(args.out_dir / "lab1_o_fig3_8B_cdf_zoom.png", dpi=300, bbox_inches="tight")
    zoom_fig.savefig(args.out_dir / "lab1_o_fig3_8B_cdf_zoom.pdf", bbox_inches="tight")
    plt.close(zoom_fig)

    write_summary(args.out_dir, data)
    print(f"Wrote paper-style lab1_o Figure 3 plots to {args.out_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
