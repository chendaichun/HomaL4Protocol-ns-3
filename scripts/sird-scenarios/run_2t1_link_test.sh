#!/usr/bin/env bash
set -euo pipefail

# 2T1 link test runner (2 senders -> 1 receiver)
# Usage:
#   ./scripts/sird-scenarios/run_2t1_link_test.sh fast
#   ./scripts/sird-scenarios/run_2t1_link_test.sh full
#
# Note:
#   This script runs scratch/HomaL4Protocol-2t1-link-test.cc via waf.

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT_DIR"

export LD_LIBRARY_PATH="$ROOT_DIR/build/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

PROFILE="${1:-fast}"
SIM_NAME="HomaL4Protocol-2t1-link-test"
OUT_DIR="outputs/sird-scenarios/${SIM_NAME}"
mkdir -p "$OUT_DIR"

# fast/full profile
START_SEC="0.2"
DURATION_SEC="0.05"
MSG_SIZE_BYTES="500000"
SEND_INTERVAL_US="400"

if [[ "$PROFILE" == "full" ]]; then
  START_SEC="1.0"
  DURATION_SEC="0.3"
  MSG_SIZE_BYTES="10000000"
  SEND_INTERVAL_US="200"
fi

run_with_timer() {
  local label="$1"
  shift
  local ts0 ts1
  ts0=$(date +%s)
  echo "[$label] start: $(date '+%F %T')"
  "$@"
  ts1=$(date +%s)
  echo "[$label] done in $((ts1 - ts0))s"
}

if [[ ! -f "scratch/${SIM_NAME}.cc" ]]; then
  echo "[error] missing scenario source: scratch/${SIM_NAME}.cc"
  exit 1
fi

TOTAL0=$(date +%s)

run_with_timer "2t1_homa" \
  ./waf --run "${SIM_NAME} --simTag=homa --enableSird=0 --startSec=${START_SEC} --durationSec=${DURATION_SEC} --msgSizeBytes=${MSG_SIZE_BYTES} --sendIntervalUs=${SEND_INTERVAL_US}"

run_with_timer "2t1_sird" \
  ./waf --run "${SIM_NAME} --simTag=sird --enableSird=1 --traceSirdCredit=1 --startSec=${START_SEC} --durationSec=${DURATION_SEC} --msgSizeBytes=${MSG_SIZE_BYTES} --sendIntervalUs=${SEND_INTERVAL_US}"

TOTAL1=$(date +%s)
echo "[2t1 total] $((TOTAL1 - TOTAL0))s"
echo "Done. Outputs should be under ${OUT_DIR}."
