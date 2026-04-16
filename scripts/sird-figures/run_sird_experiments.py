#!/usr/bin/env python3
"""Batch runner for Homa vs Homa+SIRD paper-reproduction experiments.

This script wraps:
  ./waf --run "scratch/HomaL4Protocol-paper-reproduction ..."

Example:
  python3 scripts/sird-figures/run_sird_experiments.py \
    --project-root . \
    --loads 0.1,0.3,0.5 \
    --sim-idx 0,1,2 \
    --duration 0.02 \
    --modes homa,sird \
    --trace-sird-grant
"""

from __future__ import annotations

import argparse
import shlex
import subprocess
import sys
from pathlib import Path


def parse_csv_floats(text: str) -> list[float]:
    return [float(x.strip()) for x in text.split(",") if x.strip()]


def parse_csv_ints(text: str) -> list[int]:
    return [int(x.strip()) for x in text.split(",") if x.strip()]


def run_command(cmd: str, cwd: Path) -> int:
    print(f"[RUN] {cmd}")
    proc = subprocess.run(cmd, cwd=cwd, shell=True)
    return proc.returncode


def build_ns3_run_args(args: argparse.Namespace, mode: str, load: float, sim_idx: int) -> str:
    run_args: list[str] = [
        "scratch/HomaL4Protocol-paper-reproduction",
        f"--duration={args.duration}",
        f"--load={load}",
        f"--simIdx={sim_idx}",
        f"--bdpPkts={args.bdp_pkts}",
        f"--inboundRtxTimeout={args.inbound_rtx_timeout_us}",
        f"--outboundRtxTimeout={args.outbound_rtx_timeout_us}",
    ]

    if args.disable_rtx:
        run_args.append("--disableRtx")
    if args.trace_queues:
        run_args.append("--traceQueues")

    if mode == "sird":
        run_args.extend(
            [
                "--enableSird=1",
                f"--sirdCreditBudgetPkts={args.sird_credit_budget_pkts}",
                f"--sirdUnschThresholdPkts={args.sird_unsch_threshold_pkts}",
                f"--sirdEcnMdFactor={args.sird_ecn_md_factor}",
                f"--sirdEcnAiStep={args.sird_ecn_ai_step}",
                f"--sirdSenderMdFactor={args.sird_sender_md_factor}",
                f"--sirdSenderAiStep={args.sird_sender_ai_step}",
                f"--sirdEcnAlphaGain={args.sird_ecn_alpha_gain}",
                f"--sirdSenderCsnThresholdUs={args.sird_sender_csn_threshold_us}",
            ]
        )
        if args.trace_sird_grant:
            run_args.append("--traceSirdGrant=1")

    return " ".join(shlex.quote(x) for x in run_args)


def main() -> int:
    parser = argparse.ArgumentParser(description="Run Homa/SIRD ns-3 experiments in batch")
    parser.add_argument("--project-root", default=".", help="Path to HomaL4Protocol-ns-3 root")
    parser.add_argument("--loads", default="0.1,0.3,0.5", help="Comma-separated loads")
    parser.add_argument("--sim-idx", default="0,1,2", help="Comma-separated simulation indices")
    parser.add_argument("--modes", default="homa,sird", help="Comma-separated modes: homa,sird")
    parser.add_argument("--duration", type=float, default=0.02, help="Simulation duration in seconds")

    parser.add_argument("--disable-rtx", action="store_true", help="Disable retransmission timers")
    parser.add_argument("--trace-queues", action="store_true", help="Enable queue tracing")
    parser.add_argument("--trace-sird-grant", action="store_true", help="Enable SIRD grant decision tracing")

    parser.add_argument("--bdp-pkts", type=int, default=7)
    parser.add_argument("--inbound-rtx-timeout-us", type=int, default=1000)
    parser.add_argument("--outbound-rtx-timeout-us", type=int, default=10000)

    parser.add_argument("--sird-credit-budget-pkts", type=int, default=12)
    parser.add_argument("--sird-unsch-threshold-pkts", type=int, default=12)
    parser.add_argument("--sird-ecn-md-factor", type=float, default=0.85)
    parser.add_argument("--sird-ecn-ai-step", type=float, default=1.0)
    parser.add_argument("--sird-sender-md-factor", type=float, default=0.8)
    parser.add_argument("--sird-sender-ai-step", type=float, default=1.0)
    parser.add_argument("--sird-ecn-alpha-gain", type=float, default=0.125)
    parser.add_argument("--sird-sender-csn-threshold-us", type=int, default=20)

    args = parser.parse_args()

    root = Path(args.project_root).resolve()
    if not (root / "waf").exists():
        print(f"ERROR: could not find waf under {root}", file=sys.stderr)
        return 2

    loads = parse_csv_floats(args.loads)
    sim_indices = parse_csv_ints(args.sim_idx)
    modes = [m.strip().lower() for m in args.modes.split(",") if m.strip()]

    valid_modes = {"homa", "sird"}
    for mode in modes:
        if mode not in valid_modes:
            print(f"ERROR: unsupported mode '{mode}', expected one of {sorted(valid_modes)}", file=sys.stderr)
            return 2

    total = len(loads) * len(sim_indices) * len(modes)
    done = 0

    for load in loads:
        for sim_idx in sim_indices:
            for mode in modes:
                ns3_args = build_ns3_run_args(args, mode, load, sim_idx)
                cmd = f"./waf --run \"{ns3_args}\""
                rc = run_command(cmd, root)
                done += 1
                print(f"[PROGRESS] {done}/{total} finished (mode={mode}, load={load}, simIdx={sim_idx}, rc={rc})")
                if rc != 0:
                    print("ERROR: run failed, aborting.", file=sys.stderr)
                    return rc

    print("All experiments completed.")
    print(f"Traces are under: {root / 'outputs' / 'homa-paper-reproduction'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
