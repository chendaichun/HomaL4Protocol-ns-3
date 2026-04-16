#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT_DIR"

export LD_LIBRARY_PATH="$ROOT_DIR/build/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
mkdir -p outputs/sird-scenarios

BIN_PATH="build/scratch/HomaL4Protocol-sird-microbench"
if [[ ! -x "$BIN_PATH" ]]; then
  echo "Binary not found, building once..."
  ./waf build
fi

# Paper-aligned core parameters (Section 6.1):
# BDP=24 pkts, B=1.5*BDP=36 pkts, UnschT=1*BDP=24 pkts, SThr~0.5*BDP (~9us at 100Gbps)
RTT_PKTS=24
SIRD_BUDGET=36
SIRD_UNSCH=24
SIRD_CSN_US=9

START_SEC=1.0
DURATION_SEC=0.8
PROBE_INTERVAL_US=200

run_with_timer() {
  local label="$1"
  shift
  local start_ts end_ts elapsed
  start_ts=$(date +%s)
  echo "[$label] start: $(date '+%F %T')"
  "$@"
  end_ts=$(date +%s)
  elapsed=$((end_ts - start_ts))
  echo "[$label] done in ${elapsed}s"
}

run_case() {
  local tag="$1"
  local probe_size="$2"
  local bg_rate_gbps="$3"
  local use_srr="$4"

  run_with_timer "$tag" "$BIN_PATH" \
    --scenario=receiver-congestion \
    --simTag="$tag" \
    --enableSird=1 \
    --traceSirdGrant=1 \
    --useSrrScheduling="$use_srr" \
    --rttPkts="$RTT_PKTS" \
    --sirdCreditBudgetPkts="$SIRD_BUDGET" \
    --sirdUnschThresholdPkts="$SIRD_UNSCH" \
    --sirdSenderCsnThresholdUs="$SIRD_CSN_US" \
    --startSec="$START_SEC" \
    --durationSec="$DURATION_SEC" \
    --backgroundMsgSizeBytes=10000000 \
    --backgroundRateGbps="$bg_rate_gbps" \
    --probeMsgSizeBytes="$probe_size" \
    --probeIntervalUs="$PROBE_INTERVAL_US"
}

script_start_ts=$(date +%s)

# 8B: unloaded vs incast (SRPT)
run_case "fig3_unloaded_8B" 8 0.001 0
run_case "fig3_incast_8B_srpt" 8 17.0 0

# 500KB: unloaded, incast-SRPT, incast-SRR
run_case "fig3_unloaded_500KB" 500000 0.001 0
run_case "fig3_incast_500KB_srpt" 500000 17.0 0
run_case "fig3_incast_500KB_srr" 500000 17.0 1

script_end_ts=$(date +%s)
echo "[paper fig3 total] $((script_end_ts - script_start_ts))s"
echo "Done. Traces are under outputs/sird-scenarios/."
