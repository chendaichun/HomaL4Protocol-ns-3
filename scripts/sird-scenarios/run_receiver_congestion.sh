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

script_start_ts=$(date +%s)

echo "[1/2] Receiver Congestion: probe=8B"
run_with_timer "incast_probe_8B" "$BIN_PATH" \
  --scenario=receiver-congestion \
  --simTag=incast_probe_8B \
  --enableSird=1 \
  --traceSirdCredit=1 \
  --startSec="$START_SEC" \
  --durationSec="$DURATION_SEC" \
  --backgroundMsgSizeBytes=10000000 \
  --backgroundRateGbps=17.0 \
  --probeMsgSizeBytes=8 \
  --probeIntervalUs="$PROBE_INTERVAL_US"

echo "[2/2] Receiver Congestion: probe=500KB"
run_with_timer "incast_probe_500KB" "$BIN_PATH" \
  --scenario=receiver-congestion \
  --simTag=incast_probe_500KB \
  --enableSird=1 \
  --traceSirdCredit=1 \
  --startSec="$START_SEC" \
  --durationSec="$DURATION_SEC" \
  --backgroundMsgSizeBytes=10000000 \
  --backgroundRateGbps=17.0 \
  --probeMsgSizeBytes=500000 \
  --probeIntervalUs="$PROBE_INTERVAL_US"

script_end_ts=$(date +%s)
echo "[receiver-congestion total] $((script_end_ts - script_start_ts))s"
echo "Done. Traces are under outputs/sird-scenarios/."