#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT_DIR"

export LD_LIBRARY_PATH="$ROOT_DIR/build/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
mkdir -p outputs/sird-scenarios

PROFILE="${1:-fast}"
MODE="${2:-serial}"
START_SEC="0.1"
DURATION_SEC="0.08"
PROBE_INTERVAL_US="400"

if [[ "$PROFILE" == "full" ]]; then
  START_SEC="1.0"
  DURATION_SEC="0.3"
  PROBE_INTERVAL_US="200"
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

run_case() {
  local cc_label="$1"
  local enable_sird="$2"
  local probe_size="$3"

  run_with_timer "$cc_label" "$BIN_PATH" \
    --scenario=receiver-congestion \
    --simTag="$cc_label" \
    --enableSird="$enable_sird" \
    --traceSirdCredit="$enable_sird" \
    --startSec="$START_SEC" \
    --durationSec="$DURATION_SEC" \
    --backgroundMsgSizeBytes=10000000 \
    --backgroundRateGbps=17.0 \
    --probeMsgSizeBytes="$probe_size" \
    --probeIntervalUs="$PROBE_INTERVAL_US"
}

script_start_ts=$(date +%s)

if [[ "$MODE" == "parallel" ]]; then
  echo "Running in parallel mode"
  run_case "receiver-congestion_incast_sird_probe_8B" 1 8 &
  p1=$!
  run_case "receiver-congestion_incast_homa_probe_8B" 0 8 &
  p2=$!
  run_case "receiver-congestion_incast_sird_probe_500KB" 1 500000 &
  p3=$!
  run_case "receiver-congestion_incast_homa_probe_500KB" 0 500000 &
  p4=$!

  wait "$p1"
  wait "$p2"
  wait "$p3"
  wait "$p4"
else
  echo "[1/4] SIRD probe=8B"
  run_case "receiver-congestion_incast_sird_probe_8B" 1 8

  echo "[2/4] Homa probe=8B"
  run_case "receiver-congestion_incast_homa_probe_8B" 0 8

  echo "[3/4] SIRD probe=500KB"
  run_case "receiver-congestion_incast_sird_probe_500KB" 1 500000

  echo "[4/4] Homa probe=500KB"
  run_case "receiver-congestion_incast_homa_probe_500KB" 0 500000
fi

script_end_ts=$(date +%s)
echo "[receiver-congestion compare total] $((script_end_ts - script_start_ts))s"
echo "Done. Comparison traces are under outputs/sird-scenarios/."
