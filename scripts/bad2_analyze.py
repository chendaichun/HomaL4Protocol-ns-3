#!/usr/bin/env python3
import argparse
import csv
from pathlib import Path
from statistics import mean


def parse_kv_file(path):
    data = {}
    if not path.exists():
        return data
    for line in path.read_text().splitlines():
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        data[key.strip()] = value.strip()
    return data


def parse_tokens(line):
    tokens = {}
    parts = line.strip().split()
    if parts:
        tokens["time_ns"] = parts[0]
    for part in parts[1:]:
        if "=" not in part:
            continue
        key, value = part.split("=", 1)
        tokens[key] = value
    return tokens


def as_float(value, default=0.0):
    try:
        return float(value)
    except (TypeError, ValueError):
        return default


def steady_window(summary, warmup_fraction):
    start_sec = as_float(summary.get("startSec"))
    duration_sec = as_float(summary.get("durationSec"))
    start_ns = int((start_sec + duration_sec * warmup_fraction) * 1e9)
    end_ns = int((start_sec + duration_sec) * 1e9)
    return start_ns, end_ns


def read_goodput(path, summary, sender_a, sender_b):
    duration_sec = as_float(summary.get("durationSec"), 1.0)
    completed = {sender_a: 0.0, sender_b: 0.0}
    samples = {"aggregate": [], sender_a: [], sender_b: []}

    if not path.exists():
        return completed, samples

    for line in path.read_text().splitlines():
        row = parse_tokens(line)
        sender = row.get("sender")
        if sender in samples:
            samples[sender].append(as_float(row.get("goodputGbps")))
        if sender in completed:
            completed[sender] = as_float(row.get("completedBytes"))

    avg_from_completed = {
        sender: (bytes_done * 8.0 / duration_sec / 1e9)
        for sender, bytes_done in completed.items()
    }
    return avg_from_completed, samples


def read_credit(path, summary, sender_a, sender_b, warmup_fraction):
    start_ns, end_ns = steady_window(summary, warmup_fraction)
    receiver_avail = {sender_a: [], sender_b: []}
    sender_credit = {sender_a: [], sender_b: []}
    sender_avail = {sender_a: [], sender_b: []}
    sender_budget = {sender_a: [], sender_b: []}

    if not path.exists():
        return receiver_avail, sender_credit, sender_avail, sender_budget

    for line in path.read_text().splitlines():
        row = parse_tokens(line)
        sender = row.get("sender")
        if sender not in receiver_avail:
            continue
        time_ns = int(as_float(row.get("time_ns")))
        if time_ns < start_ns or time_ns > end_ns:
            continue
        receiver_avail[sender].append(as_float(row.get("receiverAvailPkts")))
        sender_credit[sender].append(as_float(row.get("senderCreditPkts")))
        sender_avail[sender].append(as_float(row.get("senderAvailPkts")))
        sender_budget[sender].append(as_float(row.get("senderBudgetPkts")))

    return receiver_avail, sender_credit, sender_avail, sender_budget


def read_receiver_queue(path, summary, warmup_fraction):
    start_ns, end_ns = steady_window(summary, warmup_fraction)
    values = []

    if not path.exists():
        return values

    for line in path.read_text().splitlines():
        row = parse_tokens(line)
        if row.get("queue") != "switch1_to_receiver":
            continue
        time_ns = int(as_float(row.get("time_ns")))
        if time_ns < start_ns or time_ns > end_ns:
            continue
        values.append(as_float(row.get("packets")))

    return values


def avg(values):
    return mean(values) if values else 0.0


def summarize_case(summary_path, warmup_fraction):
    summary = parse_kv_file(summary_path)
    inferred_tag = summary_path.name.removeprefix("bad2_").removesuffix(".summary.tr")
    tag = summary.get("simTag", inferred_tag)
    sender_a = summary.get("senderA", "")
    sender_b = summary.get("senderB", "")
    bottleneck = as_float(summary.get("bottleneckRateGbps"))
    extra_delay = as_float(summary.get("longPathExtraDelayUs"))

    prefix = summary_path.parent / summary_path.name.removesuffix(".summary.tr")
    goodput, goodput_samples = read_goodput(prefix.with_suffix(".goodput.tr"),
                                            summary,
                                            sender_a,
                                            sender_b)
    receiver_avail, sender_credit, sender_avail, sender_budget = read_credit(
        prefix.with_suffix(".credit-sample.tr"),
        summary,
        sender_a,
        sender_b,
        warmup_fraction)
    receiver_queue = read_receiver_queue(prefix.with_suffix(".switch-egress-queue.tr"),
                                         summary,
                                         warmup_fraction)

    sender_a_goodput_gbps = goodput.get(sender_a, 0.0)
    sender_b_goodput_gbps = goodput.get(sender_b, 0.0)
    aggregate_goodput_gbps = sender_a_goodput_gbps + sender_b_goodput_gbps
    aggregate_utilization = aggregate_goodput_gbps / bottleneck if bottleneck else 0.0
    receiver_avail_a = avg(receiver_avail.get(sender_a, []))
    receiver_avail_b = avg(receiver_avail.get(sender_b, []))
    receiver_avail_ratio_a_to_b = receiver_avail_a / receiver_avail_b if receiver_avail_b else 0.0
    sender_budget_a = avg(sender_budget.get(sender_a, []))
    sender_budget_b = avg(sender_budget.get(sender_b, []))
    sender_budget_ratio_a_to_b = sender_budget_a / sender_budget_b if sender_budget_b else 0.0

    return {
        "case": tag,
        "long_path_extra_delay_us": extra_delay,
        "bottleneck_gbps": bottleneck,
        "aggregate_goodput_gbps": aggregate_goodput_gbps,
        "aggregate_utilization": aggregate_utilization,
        "sender_a_goodput_gbps": sender_a_goodput_gbps,
        "sender_b_goodput_gbps": sender_b_goodput_gbps,
        "sender_a_share": sender_a_goodput_gbps / aggregate_goodput_gbps if aggregate_goodput_gbps else 0.0,
        "sender_b_share": sender_b_goodput_gbps / aggregate_goodput_gbps if aggregate_goodput_gbps else 0.0,
        "receiver_avail_a_pkts": receiver_avail_a,
        "receiver_avail_b_pkts": receiver_avail_b,
        "receiver_avail_ratio_a_to_b": receiver_avail_ratio_a_to_b,
        "sender_avail_a_pkts": avg(sender_avail.get(sender_a, [])),
        "sender_avail_b_pkts": avg(sender_avail.get(sender_b, [])),
        "sender_budget_a_pkts": sender_budget_a,
        "sender_budget_b_pkts": sender_budget_b,
        "sender_budget_ratio_a_to_b": sender_budget_ratio_a_to_b,
        "sender_credit_a_pkts": avg(sender_credit.get(sender_a, [])),
        "sender_credit_b_pkts": avg(sender_credit.get(sender_b, [])),
        "receiver_queue_avg_pkts": avg(receiver_queue),
        "receiver_queue_peak_pkts": max(receiver_queue) if receiver_queue else 0.0,
        "aggregate_sample_avg_gbps": avg(goodput_samples.get("aggregate", [])),
    }


def write_csv(path, rows):
    fields = [
        "case",
        "long_path_extra_delay_us",
        "bottleneck_gbps",
        "aggregate_goodput_gbps",
        "aggregate_utilization",
        "sender_a_goodput_gbps",
        "sender_b_goodput_gbps",
        "sender_a_share",
        "sender_b_share",
        "receiver_avail_a_pkts",
        "receiver_avail_b_pkts",
        "receiver_avail_ratio_a_to_b",
        "sender_avail_a_pkts",
        "sender_avail_b_pkts",
        "sender_budget_a_pkts",
        "sender_budget_b_pkts",
        "sender_budget_ratio_a_to_b",
        "sender_credit_a_pkts",
        "sender_credit_b_pkts",
        "receiver_queue_avg_pkts",
        "receiver_queue_peak_pkts",
        "aggregate_sample_avg_gbps",
    ]
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fields)
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


def write_markdown(path, rows):
    path.parent.mkdir(parents=True, exist_ok=True)
    lines = [
        "# bad2 RTT Heterogeneity Analysis",
        "",
        "The main signal is aggregate utilization below the receiver bottleneck while the receiver-facing queue remains low.",
        "",
        "| extra delay (us) | aggregate Gbps | utilization | sender A Gbps | sender B Gbps | sender budget A/B | avg rx queue (pkts) | peak rx queue (pkts) |",
        "|---:|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for row in rows:
        lines.append(
            "| {long_path_extra_delay_us:.3g} | {aggregate_goodput_gbps:.3f} | "
            "{aggregate_utilization:.3f} | {sender_a_goodput_gbps:.3f} | "
            "{sender_b_goodput_gbps:.3f} | {sender_budget_ratio_a_to_b:.3f} | "
            "{receiver_queue_avg_pkts:.3f} | {receiver_queue_peak_pkts:.3f} |".format(**row)
        )
    lines.extend([
        "",
        "Interpretation guide:",
        "",
        "- If sender budget A/B is near 1.0, the receiver is allocating per-sender credit budget approximately evenly.",
        "- If sender A goodput drops as extra RTT increases while sender B does not fully compensate, credit assigned to A is turning over inefficiently.",
        "- If aggregate utilization falls below 1.0 while receiver queue stays low, the receiver bottleneck is under-utilized rather than congested.",
    ])
    path.write_text("\n".join(lines) + "\n")


def write_plots(out_dir, rows):
    if not rows:
        return

    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    out_dir.mkdir(parents=True, exist_ok=True)
    xs = [row["long_path_extra_delay_us"] for row in rows]
    baseline = rows[0]
    baseline_utilization_pct = baseline["aggregate_utilization"] * 100.0
    bottleneck_gbps = baseline["bottleneck_gbps"]

    plt.figure(figsize=(6.2, 4.0))
    plt.plot(xs, [row["aggregate_utilization"] * 100.0 for row in rows],
             marker="o", label="aggregate utilization")
    plt.axhline(100.0, color="0.45", linewidth=1.0, linestyle="--", label="100% bottleneck")
    plt.axhline(baseline_utilization_pct, color="#2f6f9f", linewidth=1.0,
                linestyle=":", label=f"0us baseline ({baseline_utilization_pct:.1f}%)")
    plt.xlabel("Sender A extra one-way delay (us)")
    plt.ylabel("Receiver bottleneck utilization (%)")
    plt.ylim(0, 110)
    plt.grid(True, alpha=0.3)
    plt.legend()
    plt.tight_layout()
    plt.savefig(out_dir / "aggregate_utilization_vs_rtt.png", dpi=180)
    plt.close()

    plt.figure(figsize=(6.2, 4.0))
    utilization = [row["aggregate_utilization"] * 100.0 for row in rows]
    loss_vs_baseline = [baseline_utilization_pct - value for value in utilization]
    plt.bar(xs, utilization, width=2.2, label="measured utilization")
    plt.plot(xs, [baseline_utilization_pct for _ in rows], color="#2f6f9f",
             linewidth=1.5, linestyle=":", label=f"0us baseline ({baseline_utilization_pct:.1f}%)")
    plt.plot(xs, [100.0 for _ in rows], color="0.35",
             linewidth=1.0, linestyle="--", label="100% bottleneck")
    for x, value, loss in zip(xs, utilization, loss_vs_baseline):
        if loss > 0.5:
            plt.text(x, value + 2, f"-{loss:.1f}pp", ha="center", fontsize=8)
    plt.xlabel("Sender A extra one-way delay (us)")
    plt.ylabel("Receiver bottleneck utilization (%)")
    plt.ylim(0, 110)
    plt.grid(True, axis="y", alpha=0.3)
    plt.legend()
    plt.tight_layout()
    plt.savefig(out_dir / "aggregate_utilization_baseline_comparison.png", dpi=180)
    plt.close()

    plt.figure(figsize=(6.2, 4.0))
    plt.plot(xs, [row["sender_a_goodput_gbps"] for row in rows],
             marker="o", label="sender A (long RTT)")
    plt.plot(xs, [row["sender_b_goodput_gbps"] for row in rows],
             marker="s", label="sender B (short RTT)")
    plt.plot(xs, [row["aggregate_goodput_gbps"] for row in rows],
             marker="^", label="aggregate", color="0.2")
    plt.axhline(rows[0]["bottleneck_gbps"], color="0.45", linewidth=1.0,
                linestyle="--", label="receiver bottleneck")
    plt.xlabel("Sender A extra one-way delay (us)")
    plt.ylabel("Goodput (Gbps)")
    plt.grid(True, alpha=0.3)
    plt.legend()
    plt.tight_layout()
    plt.savefig(out_dir / "sender_goodput_vs_rtt.png", dpi=180)
    plt.close()

    plt.figure(figsize=(6.2, 4.0))
    sender_a = [row["sender_a_goodput_gbps"] for row in rows]
    sender_b = [row["sender_b_goodput_gbps"] for row in rows]
    unused = [max(0.0, row["bottleneck_gbps"] - row["aggregate_goodput_gbps"]) for row in rows]
    plt.bar(xs, sender_a, width=2.2, label="sender A goodput")
    plt.bar(xs, sender_b, width=2.2, bottom=sender_a, label="sender B goodput")
    plt.bar(xs, unused, width=2.2,
            bottom=[a + b for a, b in zip(sender_a, sender_b)],
            label="unused bottleneck capacity", color="0.82")
    plt.axhline(bottleneck_gbps, color="0.35", linewidth=1.0,
                linestyle="--", label="receiver bottleneck")
    plt.xlabel("Sender A extra one-way delay (us)")
    plt.ylabel("Goodput / capacity (Gbps)")
    plt.ylim(0, bottleneck_gbps * 1.12)
    plt.grid(True, axis="y", alpha=0.3)
    plt.legend()
    plt.tight_layout()
    plt.savefig(out_dir / "goodput_stack_with_unused_capacity.png", dpi=180)
    plt.close()

    plt.figure(figsize=(6.2, 4.0))
    baseline_a = baseline["sender_a_goodput_gbps"] or 1.0
    baseline_b = baseline["sender_b_goodput_gbps"] or 1.0
    baseline_aggregate = baseline["aggregate_goodput_gbps"] or 1.0
    plt.plot(xs, [row["sender_a_goodput_gbps"] / baseline_a * 100.0 for row in rows],
             marker="o", label="sender A vs 0us")
    plt.plot(xs, [row["sender_b_goodput_gbps"] / baseline_b * 100.0 for row in rows],
             marker="s", label="sender B vs 0us")
    plt.plot(xs, [row["aggregate_goodput_gbps"] / baseline_aggregate * 100.0 for row in rows],
             marker="^", color="0.2", label="aggregate vs 0us")
    plt.axhline(100.0, color="#2f6f9f", linewidth=1.0,
                linestyle=":", label="0us baseline")
    plt.xlabel("Sender A extra one-way delay (us)")
    plt.ylabel("Goodput relative to 0us baseline (%)")
    plt.grid(True, alpha=0.3)
    plt.legend()
    plt.tight_layout()
    plt.savefig(out_dir / "normalized_goodput_vs_baseline.png", dpi=180)
    plt.close()

    plt.figure(figsize=(6.2, 4.0))
    plt.plot(xs, [row["sender_budget_ratio_a_to_b"] for row in rows],
             marker="o", label="sender budget A/B")
    plt.axhline(1.0, color="0.45", linewidth=1.0, linestyle="--", label="equal budget")
    plt.xlabel("Sender A extra one-way delay (us)")
    plt.ylabel("Credit budget ratio")
    plt.ylim(0, max(1.2, max(row["sender_budget_ratio_a_to_b"] for row in rows) * 1.2))
    plt.grid(True, alpha=0.3)
    plt.legend()
    plt.tight_layout()
    plt.savefig(out_dir / "sender_budget_ratio_vs_rtt.png", dpi=180)
    plt.close()

    plt.figure(figsize=(6.2, 4.0))
    plt.plot(xs, [row["receiver_queue_avg_pkts"] for row in rows],
             marker="o", label="avg receiver queue")
    plt.plot(xs, [row["receiver_queue_peak_pkts"] for row in rows],
             marker="s", label="peak receiver queue")
    plt.xlabel("Sender A extra one-way delay (us)")
    plt.ylabel("Receiver-facing queue (packets)")
    plt.grid(True, alpha=0.3)
    plt.legend()
    plt.tight_layout()
    plt.savefig(out_dir / "receiver_queue_vs_rtt.png", dpi=180)
    plt.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--out-csv", required=True, type=Path)
    parser.add_argument("--out-md", required=True, type=Path)
    parser.add_argument("--plot-dir", type=Path)
    parser.add_argument("--warmup-fraction", default=0.2, type=float)
    args = parser.parse_args()

    rows = []
    for summary_path in sorted(args.root.glob("bad2_*.summary.tr")):
        row = summarize_case(summary_path, args.warmup_fraction)
        if row is not None:
            rows.append(row)
    rows.sort(key=lambda row: row["long_path_extra_delay_us"])

    write_csv(args.out_csv, rows)
    write_markdown(args.out_md, rows)
    if args.plot_dir is not None:
        write_plots(args.plot_dir, rows)
    print(f"wrote {args.out_csv}")
    print(f"wrote {args.out_md}")
    if args.plot_dir is not None:
        print(f"wrote plots to {args.plot_dir}")


if __name__ == "__main__":
    main()
