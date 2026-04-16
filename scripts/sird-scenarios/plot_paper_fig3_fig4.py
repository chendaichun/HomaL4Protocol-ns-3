#!/usr/bin/env python3
import argparse
import os
from collections import Counter, defaultdict

import matplotlib.pyplot as plt


def parse_msg_trace(file_path, expected_size):
    begin = {}
    lat_us = []
    with open(file_path, "r", encoding="utf-8") as f:
        for line in f:
            p = line.strip().split()
            if len(p) < 6:
                continue
            flag = p[0]
            t_ns = int(p[1])
            size = int(p[2])
            src = p[3]
            dst = p[4]
            tx = int(p[5])
            if size != expected_size:
                continue
            key = (src, dst, tx, size)
            if flag == "+":
                begin[key] = t_ns
            elif flag == "-":
                st = begin.pop(key, None)
                if st is not None and t_ns >= st:
                    lat_us.append((t_ns - st) / 1000.0)
    lat_us.sort()
    return lat_us


def cdf_xy(vals):
    if not vals:
        return [], []
    n = len(vals)
    return vals, [(i + 1) / n for i in range(n)]


def find_first_existing(base_dir, names):
    for n in names:
        p = os.path.join(base_dir, n)
        if os.path.exists(p):
            return p
    return None


def plot_fig3(trace_dir, out_dir):
    # Paper-like naming from run_paper_fig3.sh
    files = {
        "unloaded_8b": ["receiver-congestion_fig3_unloaded_8B.msg.tr"],
        "incast_8b": ["receiver-congestion_fig3_incast_8B_srpt.msg.tr"],
        "unloaded_500k": ["receiver-congestion_fig3_unloaded_500KB.msg.tr"],
        "incast_500k_srpt": ["receiver-congestion_fig3_incast_500KB_srpt.msg.tr"],
        "incast_500k_srr": ["receiver-congestion_fig3_incast_500KB_srr.msg.tr"],
    }

    paths = {k: find_first_existing(trace_dir, v) for k, v in files.items()}
    required = ["unloaded_8b", "incast_8b", "unloaded_500k", "incast_500k_srpt", "incast_500k_srr"]
    if any(paths[k] is None for k in required):
        missing = [k for k in required if paths[k] is None]
        return None, f"missing fig3 traces: {missing}"

    unloaded_8 = parse_msg_trace(paths["unloaded_8b"], 8)
    incast_8 = parse_msg_trace(paths["incast_8b"], 8)
    unloaded_500 = parse_msg_trace(paths["unloaded_500k"], 500000)
    incast_500_srpt = parse_msg_trace(paths["incast_500k_srpt"], 500000)
    incast_500_srr = parse_msg_trace(paths["incast_500k_srr"], 500000)

    fig, axes = plt.subplots(1, 2, figsize=(11.5, 4.8))

    x, y = cdf_xy(unloaded_8)
    axes[0].plot(x, y, label="Unloaded", linewidth=2.2)
    x, y = cdf_xy(incast_8)
    axes[0].plot(x, y, label="Incast", linewidth=2.2)
    axes[0].set_xlabel("Latency (us)")
    axes[0].set_ylabel("CDF")
    axes[0].set_title("8B requests")
    axes[0].grid(True, alpha=0.3)
    axes[0].legend()

    x, y = cdf_xy(unloaded_500)
    axes[1].plot(x, y, label="Unloaded", linewidth=2.2)
    x, y = cdf_xy(incast_500_srpt)
    axes[1].plot(x, y, label="Incast-SRPT", linewidth=2.2)
    x, y = cdf_xy(incast_500_srr)
    axes[1].plot(x, y, label="Incast-SRR", linewidth=2.2)
    axes[1].set_xlabel("Latency (us)")
    axes[1].set_ylabel("CDF")
    axes[1].set_title("500KB requests")
    axes[1].grid(True, alpha=0.3)
    axes[1].legend()

    fig.suptitle("Figure 3 Reproduction: Incast latency CDF", y=1.02)
    fig.tight_layout()
    out_path = os.path.join(out_dir, "paper_fig3_incast_latency_cdf.png")
    fig.savefig(out_path, dpi=170, bbox_inches="tight")
    plt.close(fig)
    return out_path, None


def moving_average(values, win):
    if win <= 1:
        return values[:]
    out = []
    s = 0.0
    q = []
    for v in values:
        q.append(v)
        s += v
        if len(q) > win:
            s -= q.pop(0)
        out.append(s / len(q))
    return out


def parse_credit_events(path):
    events = []
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            toks = line.strip().split()
            if not toks:
                continue
            t_ns = int(toks[0])
            fields = {}
            for tok in toks[1:]:
                if "=" in tok:
                    k, v = tok.split("=", 1)
                    fields[k] = v
            if "type" not in fields or "recvNode" not in fields or "sender" not in fields:
                continue
            events.append((t_ns, fields["type"], int(fields["recvNode"]), fields["sender"]))
    events.sort(key=lambda x: x[0])
    return events


def derive_fig4_series(events, bdp_pkts, budget_pkts, dt_s=0.01):
    if not events:
        return None

    grant_by_sender = Counter()
    recv_nodes = Counter()
    for _, tp, rn, sender in events:
        if tp == "grant":
            grant_by_sender[sender] += 1
            recv_nodes[rn] += 1

    if not grant_by_sender:
        return None

    target_sender = grant_by_sender.most_common(1)[0][0]
    receivers = [n for n, _ in recv_nodes.most_common(3)]
    if not receivers:
        return None

    outstanding = defaultdict(int)  # (recvNode, sender) -> credits in use
    total_by_recv = defaultdict(int)

    t0 = events[0][0]
    t_end = events[-1][0]
    sample_ns = int(dt_s * 1e9)
    next_sample = t0

    xs = []
    ys_sender_acc = []
    ys_recv_avail = []

    idx = 0
    n = len(events)
    while next_sample <= t_end:
        while idx < n and events[idx][0] <= next_sample:
            _, tp, rn, sender = events[idx]
            if tp == "grant":
                outstanding[(rn, sender)] += 1
                total_by_recv[rn] += 1
            elif tp == "data":
                key = (rn, sender)
                if outstanding[key] > 0:
                    outstanding[key] -= 1
                    total_by_recv[rn] = max(0, total_by_recv[rn] - 1)
            idx += 1

        sender_acc = sum(outstanding[(rn, target_sender)] for rn in receivers)
        recv_avail = sum(max(0, budget_pkts - total_by_recv[rn]) for rn in receivers)

        xs.append((next_sample - t0) / 1e9)
        ys_sender_acc.append(sender_acc / float(bdp_pkts))
        ys_recv_avail.append(recv_avail / float(bdp_pkts))

        next_sample += sample_ns

    # 100ms moving average
    ma_win = max(1, int(round(0.1 / dt_s)))
    ys_sender_acc = moving_average(ys_sender_acc, ma_win)
    ys_recv_avail = moving_average(ys_recv_avail, ma_win)

    return xs, ys_sender_acc, ys_recv_avail


def plot_fig4(trace_dir, out_dir, bdp_pkts, budget_pkts):
    p_half = os.path.join(trace_dir, "sender-information_fig4_sthr_half.credit-events.tr")
    p_inf = os.path.join(trace_dir, "sender-information_fig4_sthr_inf.credit-events.tr")
    if not os.path.exists(p_half) or not os.path.exists(p_inf):
        return None, "missing fig4 credit-events traces"

    s_half = derive_fig4_series(parse_credit_events(p_half), bdp_pkts, budget_pkts)
    s_inf = derive_fig4_series(parse_credit_events(p_inf), bdp_pkts, budget_pkts)
    if s_half is None or s_inf is None:
        return None, "insufficient events to derive fig4 series"

    fig, axes = plt.subplots(1, 2, figsize=(11.5, 4.8))

    axes[0].plot(s_half[0], s_half[1], label="SThr=0.5xBDP", linewidth=2.2)
    axes[0].plot(s_inf[0], s_inf[1], label="SThr=Inf", linewidth=2.2)
    axes[0].set_xlabel("Time (s)")
    axes[0].set_ylabel("Credit (xBDP)")
    axes[0].set_title("Credit accumulated at congested sender")
    axes[0].grid(True, alpha=0.3)
    axes[0].legend()

    axes[1].plot(s_half[0], s_half[2], label="SThr=0.5xBDP", linewidth=2.2)
    axes[1].plot(s_inf[0], s_inf[2], label="SThr=Inf", linewidth=2.2)
    axes[1].set_xlabel("Time (s)")
    axes[1].set_ylabel("Credit (xBDP)")
    axes[1].set_title("Sum of credit available at three receivers")
    axes[1].grid(True, alpha=0.3)
    axes[1].legend()

    fig.suptitle("Figure 4 Reproduction (100ms moving average)", y=1.02)
    fig.tight_layout()
    out_path = os.path.join(out_dir, "paper_fig4_sender_credit_dynamics.png")
    fig.savefig(out_path, dpi=170, bbox_inches="tight")
    plt.close(fig)
    return out_path, None


def main():
    parser = argparse.ArgumentParser(description="Plot paper-style Figure 3 and Figure 4 reproductions")
    parser.add_argument("--trace-dir", default="outputs/sird-scenarios")
    parser.add_argument("--out-dir", default="output-f")
    parser.add_argument("--bdp-pkts", type=int, default=24)
    parser.add_argument("--budget-pkts", type=int, default=36)
    args = parser.parse_args()

    os.makedirs(args.out_dir, exist_ok=True)

    fig3_path, fig3_warn = plot_fig3(args.trace_dir, args.out_dir)
    fig4_path, fig4_warn = plot_fig4(args.trace_dir, args.out_dir, args.bdp_pkts, args.budget_pkts)

    if fig3_path:
        print(f"Saved: {fig3_path}")
    else:
        print(f"Skip fig3: {fig3_warn}")

    if fig4_path:
        print(f"Saved: {fig4_path}")
    else:
        print(f"Skip fig4: {fig4_warn}")


if __name__ == "__main__":
    main()
