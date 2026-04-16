#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT_DIR"

export LD_LIBRARY_PATH="$ROOT_DIR/build/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

mkdir -p outputs/sird-scenarios

# Profiles:
#   fast (default): shorter runs for quick iteration
#   full: closer to paper-style setup
PROFILE="${1:-fast}"

START_SEC="0.2"
DURATION_SEC="0.05"
FLOW_GAP_US="100000"

if [[ "$PROFILE" == "full" ]]; then
  START_SEC="1.0"
  DURATION_SEC="0.6"
  FLOW_GAP_US="200000"
fi

BIN_PATH="build/scratch/HomaL4Protocol-sird-microbench"
if [[ ! -x "$BIN_PATH" ]]; then
  echo "Binary not found, building once..."
  ./waf build
fi

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

script_start_ts=$(date +%s)

echo "[1/2] Sender Information: with sender feedback (SIRD default)"
run_with_timer "outcast_sird_feedback" "$BIN_PATH" \
  --scenario=sender-information \
  --simTag=outcast_sird_feedback \
  --enableSird=1 \
  --traceSirdCredit=1 \
  --startSec="$START_SEC" \
  --durationSec="$DURATION_SEC" \
  --outcastMsgSizeBytes=10000000 \
  --outcastFlowGapUs="$FLOW_GAP_US"

echo "[2/2] Sender Information: reduced sender-feedback control (approx no informed overcommitment)"
run_with_timer "outcast_weak_sender_feedback" "$BIN_PATH" \
  --scenario=sender-information \
  --simTag=outcast_weak_sender_feedback \
  --enableSird=1 \
  --traceSirdCredit=1 \
  --startSec="$START_SEC" \
  --durationSec="$DURATION_SEC" \
  --outcastMsgSizeBytes=10000000 \
  --outcastFlowGapUs="$FLOW_GAP_US" \
  --sirdSenderMdFactor=1.0 \
  --sirdSenderAiStep=0.0 \
  --sirdSenderCsnThresholdUs=1000000000

script_end_ts=$(date +%s)
echo "[sender-information total] $((script_end_ts - script_start_ts))s"
echo "Done. Traces are under outputs/sird-scenarios/."