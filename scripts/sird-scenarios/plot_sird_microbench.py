#!/usr/bin/env python3
import argparse
import os
import re
from collections import defaultdict

import matplotlib.pyplot as plt


def parse_msg_trace(file_path, expected_size=None):
    begin = {}
    lat_us = []

    with open(file_path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            parts = line.split()
            if len(parts) < 6:
                continue

            mark = parts[0]
            t_ns = int(parts[1])
            size = int(parts[2])
            src = parts[3]
            dst = parts[4]
            tx_msg_id = int(parts[5])

            if expected_size is not None and size != expected_size:
                continue

            key = (src, dst, tx_msg_id, size)
            if mark == "+":
                begin[key] = t_ns
            elif mark == "-":
                start_t = begin.pop(key, None)
                if start_t is not None and t_ns >= start_t:
                    lat_us.append((t_ns - start_t) / 1000.0)

    lat_us.sort()
    return lat_us


def cdf_xy(values):
    if not values:
        return [], []
    n = len(values)
    xs = values
    ys = [(i + 1) / n for i in range(n)]
    return xs, ys


def parse_sird_credit(file_path):
    # format: <time_ns> sender=... txMsgId=... grantOffset=... senderBudgetPkts=... ecnEwma=... senderCsn=...
    sender_budget = defaultdict(list)
    sender_csn = defaultdict(list)

    budget_re = re.compile(r"^(\d+)\s+sender=([^\s]+).*senderBudgetPkts=([^\s]+).*senderCsn=([01])")

    with open(file_path, "r", encoding="utf-8") as f:
        for line in f:
            m = budget_re.search(line.strip())
            if not m:
                continue
            t_ns = int(m.group(1))
            sender = m.group(2)
            budget = float(m.group(3))
            csn = int(m.group(4))
            t_ms = t_ns / 1e6
            sender_budget[sender].append((t_ms, budget))
            sender_csn[sender].append((t_ms, csn))

    return sender_budget, sender_csn


def plot_receiver_congestion(trace_dir, out_dir):
    file_8b = os.path.join(trace_dir, "receiver-congestion_incast_probe_8B.msg.tr")
    file_500k = os.path.join(trace_dir, "receiver-congestion_incast_probe_500KB.msg.tr")

    if not os.path.exists(file_8b) and not os.path.exists(file_500k):
        return None, 0, 0, "missing receiver-congestion trace files"

    lat_8b = parse_msg_trace(file_8b, expected_size=8) if os.path.exists(file_8b) else []
    lat_500k = parse_msg_trace(file_500k, expected_size=500000) if os.path.exists(file_500k) else []

    x1, y1 = cdf_xy(lat_8b)
    x2, y2 = cdf_xy(lat_500k)

    plt.figure(figsize=(8, 5))
    if x1:
        plt.plot(x1, y1, label=f"Probe 8B (n={len(x1)})", linewidth=2)
    if x2:
        plt.plot(x2, y2, label=f"Probe 500KB (n={len(x2)})", linewidth=2)
    plt.xlabel("Latency (us)")
    plt.ylabel("CDF")
    plt.title("Receiver Congestion: Probe Message Latency CDF")
    plt.grid(True, alpha=0.3)
    plt.legend()
    out_path = os.path.join(out_dir, "receiver_congestion_probe_latency_cdf.png")
    plt.tight_layout()
    plt.savefig(out_path, dpi=160)
    plt.close()
    return out_path, len(lat_8b), len(lat_500k), None


def plot_sender_information(trace_dir, out_dir):
    default_candidates = [
        os.path.join(trace_dir, "sender-information_outcast_sird_feedback.sird-credit.tr"),
        os.path.join(trace_dir, "sender-information_outcast_sird_feedback.sird-grant.tr"),
    ]
    weak_candidates = [
        os.path.join(trace_dir, "sender-information_outcast_weak_sender_feedback.sird-credit.tr"),
        os.path.join(trace_dir, "sender-information_outcast_weak_sender_feedback.sird-grant.tr"),
    ]

    default_file = next((p for p in default_candidates if os.path.exists(p)), None)
    weak_file = next((p for p in weak_candidates if os.path.exists(p)), None)

    if default_file is None and weak_file is None:
        return None, "missing sender-information credit trace files"

    default_budget, _ = parse_sird_credit(default_file) if default_file is not None else ({}, {})
    weak_budget, _ = parse_sird_credit(weak_file) if weak_file is not None else ({}, {})

    # For this scenario we expect one sender; pick the sender with most samples.
    def pick_sender(data):
        if not data:
            return None
        return max(data.keys(), key=lambda k: len(data[k]))

    sender_default = pick_sender(default_budget)
    sender_weak = pick_sender(weak_budget)

    plt.figure(figsize=(9, 5))
    if sender_default is not None:
        xs = [p[0] for p in default_budget[sender_default]]
        ys = [p[1] for p in default_budget[sender_default]]
        plt.plot(xs, ys, label=f"SIRD feedback ({sender_default})", linewidth=1.8)
    if sender_weak is not None:
        xs = [p[0] for p in weak_budget[sender_weak]]
        ys = [p[1] for p in weak_budget[sender_weak]]
        plt.plot(xs, ys, label=f"Weak sender feedback ({sender_weak})", linewidth=1.8)

    plt.xlabel("Simulation time (ms)")
    plt.ylabel("senderBudgetPkts")
    plt.title("Sender Information (Outcast): Sender Budget Evolution")
    plt.grid(True, alpha=0.3)
    if sender_default is not None or sender_weak is not None:
        plt.legend()
    else:
        plt.text(0.5, 0.5, "No valid sender-budget samples found", ha="center", va="center", transform=plt.gca().transAxes)
    out_path = os.path.join(out_dir, "sender_information_budget_timeseries.png")
    plt.tight_layout()
    plt.savefig(out_path, dpi=160)
    plt.close()
    return out_path, None


def plot_receiver_congestion_compare(trace_dir, out_dir):
    file_map = {
        "SIRD": {
            8: [
                "receiver-congestion_incast_sird_probe_8B.msg.tr",
                "receiver-congestion_receiver-congestion_incast_sird_probe_8B.msg.tr",
            ],
            500000: [
                "receiver-congestion_incast_sird_probe_500KB.msg.tr",
                "receiver-congestion_receiver-congestion_incast_sird_probe_500KB.msg.tr",
            ],
        },
        "Homa": {
            8: [
                "receiver-congestion_incast_homa_probe_8B.msg.tr",
                "receiver-congestion_receiver-congestion_incast_homa_probe_8B.msg.tr",
            ],
            500000: [
                "receiver-congestion_incast_homa_probe_500KB.msg.tr",
                "receiver-congestion_receiver-congestion_incast_homa_probe_500KB.msg.tr",
            ],
        },
    }

    def load_vals(protocol_label, msg_size):
        vals = []
        for name in file_map[protocol_label][msg_size]:
            path = os.path.join(trace_dir, name)
            if os.path.exists(path):
                vals = parse_msg_trace(path, expected_size=msg_size)
                if vals:
                    break
        return vals

    out_paths = []
    size_to_outfile = {
        8: "receiver_congestion_homa_vs_sird_8B_cdf.png",
        500000: "receiver_congestion_homa_vs_sird_500KB_cdf.png",
    }

    for msg_size, out_name in size_to_outfile.items():
        sird_vals = load_vals("SIRD", msg_size)
        homa_vals = load_vals("Homa", msg_size)

        if not sird_vals and not homa_vals:
            continue

        plt.figure(figsize=(8.6, 5.3))
        if sird_vals:
            xs, ys = cdf_xy(sird_vals)
            plt.plot(xs, ys, label=f"SIRD (n={len(sird_vals)})", linewidth=2)
        if homa_vals:
            xs, ys = cdf_xy(homa_vals)
            plt.plot(xs, ys, label=f"Homa (n={len(homa_vals)})", linewidth=2)

        size_label = "8B" if msg_size == 8 else "500KB"
        plt.xlabel("Latency (us)")
        plt.ylabel("CDF")
        plt.title(f"Incast Comparison ({size_label}): Homa vs SIRD")
        plt.grid(True, alpha=0.3)
        plt.legend()
        out_path = os.path.join(out_dir, out_name)
        plt.tight_layout()
        plt.savefig(out_path, dpi=160)
        plt.close()
        out_paths.append(out_path)

    if not out_paths:
        return [], "missing Homa/SIRD comparison trace files for incast"
    return out_paths, None


def plot_sird_credit_share_500k(trace_dir, out_dir, bin_ms=5):
    msg_candidates = [
        "receiver-congestion_incast_sird_probe_500KB.msg.tr",
        "receiver-congestion_receiver-congestion_incast_sird_probe_500KB.msg.tr",
    ]
    credit_candidates = [
        "receiver-congestion_incast_sird_probe_500KB.sird-credit.tr",
        "receiver-congestion_receiver-congestion_incast_sird_probe_500KB.sird-credit.tr",
        "receiver-congestion_incast_sird_probe_500KB.sird-grant.tr",
        "receiver-congestion_receiver-congestion_incast_sird_probe_500KB.sird-grant.tr",
    ]

    def pick_existing(names):
        for n in names:
            p = os.path.join(trace_dir, n)
            if os.path.exists(p):
                return p
        return None

    msg_path = pick_existing(msg_candidates)
    credit_path = pick_existing(credit_candidates)

    if msg_path is None or credit_path is None:
        return None, "missing SIRD 500KB msg/credit traces"

    size_by_key = {}
    with open(msg_path, "r", encoding="utf-8") as f:
        for line in f:
            parts = line.strip().split()
            if len(parts) < 6 or parts[0] != "+":
                continue
            size = int(parts[2])
            src = parts[3].split(":")[0]
            tx_msg_id = int(parts[5])
            size_by_key[(src, tx_msg_id)] = size

    bins_total = defaultdict(int)
    bins_500k = defaultdict(int)
    bins_10m = defaultdict(int)
    t0_ns = None

    with open(credit_path, "r", encoding="utf-8") as f:
        for line in f:
            toks = line.strip().split()
            if not toks:
                continue
            t_ns = int(toks[0])
            if t0_ns is None:
                t0_ns = t_ns

            fields = {}
            for tok in toks[1:]:
                if "=" in tok:
                    k, v = tok.split("=", 1)
                    fields[k] = v

            if "sender" not in fields or "txMsgId" not in fields:
                continue

            key = (fields["sender"], int(fields["txMsgId"]))
            size = size_by_key.get(key)
            if size is None:
                continue

            b = int((t_ns - t0_ns) / (bin_ms * 1e6))
            bins_total[b] += 1
            if size == 500000:
                bins_500k[b] += 1
            if size == 10000000:
                bins_10m[b] += 1

    if not bins_total:
        return None, "no matched credit samples for SIRD 500KB run"

    xs = sorted(bins_total.keys())
    x_ms = [x * bin_ms for x in xs]
    y_500 = [100.0 * bins_500k[x] / bins_total[x] for x in xs]
    y_10m = [100.0 * bins_10m[x] / bins_total[x] for x in xs]

    plt.figure(figsize=(8.8, 5.0))
    plt.plot(x_ms, y_500, label="500KB credit share (%)", linewidth=2)
    plt.plot(x_ms, y_10m, label="10MB credit share (%)", linewidth=2)
    plt.xlabel("Time from first credit (ms)")
    plt.ylabel("Share of credits (%)")
    plt.title("SIRD Incast (500KB run): Credit Share by Message Size")
    plt.grid(True, alpha=0.3)
    plt.ylim(0, 100)
    plt.legend()
    out_path = os.path.join(out_dir, "receiver_congestion_sird_credit_share_500KB_timeseries.png")
    plt.tight_layout()
    plt.savefig(out_path, dpi=160)
    plt.close()
    return out_path, None


def main():
    parser = argparse.ArgumentParser(description="Plot SIRD microbench traces")
    parser.add_argument(
        "--trace-dir",
        default="outputs/sird-scenarios",
        help="Directory containing *.msg.tr and *.sird-credit.tr (or legacy *.sird-grant.tr) files",
    )
    parser.add_argument(
        "--out-dir",
        default="output-f",
        help="Directory to write generated figures",
    )
    args = parser.parse_args()

    os.makedirs(args.out_dir, exist_ok=True)

    cdf_path, n8, n500, cdf_warn = plot_receiver_congestion(args.trace_dir, args.out_dir)
    budget_path, budget_warn = plot_sender_information(args.trace_dir, args.out_dir)
    compare_paths, compare_warn = plot_receiver_congestion_compare(args.trace_dir, args.out_dir)
    share_path, share_warn = plot_sird_credit_share_500k(args.trace_dir, args.out_dir)

    if cdf_path is not None:
        print(f"Saved: {cdf_path}")
        print(f"Probe sample counts: 8B={n8}, 500KB={n500}")
    else:
        print(f"Skip receiver congestion plot: {cdf_warn}")

    if budget_path is not None:
        print(f"Saved: {budget_path}")
    else:
        print(f"Skip sender information plot: {budget_warn}")

    if compare_paths:
        for p in compare_paths:
            print(f"Saved: {p}")
    else:
        print(f"Skip incast Homa-vs-SIRD comparison plot: {compare_warn}")

    if share_path is not None:
        print(f"Saved: {share_path}")
    else:
        print(f"Skip SIRD 500KB credit-share plot: {share_warn}")


if __name__ == "__main__":
    main()
