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
# BDP=24 pkts, B=1.5*BDP=36 pkts, UnschT=1*BDP=24 pkts.
RTT_PKTS=24
SIRD_BUDGET=36
SIRD_UNSCH=24

START_SEC=0.2
DURATION_SEC=13.2
FLOW_GAP_US=4500000

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
  local csn_us="$2"
  local md="$3"
  local ai="$4"

  run_with_timer "$tag" "$BIN_PATH" \
    --scenario=sender-information \
    --simTag="$tag" \
    --enableSird=1 \
    --traceSirdGrant=1 \
    --traceCreditEvents=1 \
    --useSrrScheduling=0 \
    --rttPkts="$RTT_PKTS" \
    --sirdCreditBudgetPkts="$SIRD_BUDGET" \
    --sirdUnschThresholdPkts="$SIRD_UNSCH" \
    --sirdSenderCsnThresholdUs="$csn_us" \
    --sirdSenderMdFactor="$md" \
    --sirdSenderAiStep="$ai" \
    --startSec="$START_SEC" \
    --durationSec="$DURATION_SEC" \
    --outcastMsgSizeBytes=10000000 \
    --outcastFlowGapUs="$FLOW_GAP_US"
}

script_start_ts=$(date +%s)

# SThr = 0.5*BDP (approx as sender queue-drain threshold ~9us at 100Gbps)
run_case "fig4_sthr_half" 9 0.8 1.0

# SThr = Inf (disable sender-feedback loop)
run_case "fig4_sthr_inf" 1000000000 1.0 0.0

script_end_ts=$(date +%s)
echo "[paper fig4 total] $((script_end_ts - script_start_ts))s"
echo "Done. Traces are under outputs/sird-scenarios/."
