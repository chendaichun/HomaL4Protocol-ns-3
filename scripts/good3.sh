#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

PROFILE="${1:-fast}"

OFFERED_LOAD="${OFFERED_LOAD:-1.0}"
START_SEC="${START_SEC:-0.2}"
DURATION_SEC="${DURATION_SEC:-0.08}"
SETTLE_TAIL_SEC="${SETTLE_TAIL_SEC:-0.15}"
QUEUE_SAMPLE_US="${QUEUE_SAMPLE_US:-100}"
GOODPUT_SAMPLE_US="${GOODPUT_SAMPLE_US:-100}"
CORE_RATE_GBPS="${CORE_RATE_GBPS:-20}"
DEVICE_QUEUE_MAX_SIZE="${DEVICE_QUEUE_MAX_SIZE:-1p}"
QDISC_MAX_SIZE="${QDISC_MAX_SIZE:-200p}"
QDISC_MARK_THRESHOLD="${QDISC_MARK_THRESHOLD:-10p}"
WORKLOAD_FILE="${WORKLOAD_FILE:-inputs/W5_DCTCP_scaled1442_bytes_cdf.csv}"

if [[ "$PROFILE" == "smoke" ]]; then
  DURATION_SEC="${DURATION_SEC_SMOKE:-0.04}"
  SETTLE_TAIL_SEC="${SETTLE_TAIL_SEC_SMOKE:-0.08}"
elif [[ "$PROFILE" == "full" ]]; then
  DURATION_SEC="${DURATION_SEC_FULL:-0.25}"
  SETTLE_TAIL_SEC="${SETTLE_TAIL_SEC_FULL:-0.2}"
elif [[ "$PROFILE" != "fast" ]]; then
  echo "Usage: bash scripts/good3.sh [smoke|fast|full]"
  exit 2
fi

BUILD="${BUILD:-1}"
TRACE_MSG="${TRACE_MSG:-0}"
TRACE_TOR_QUEUE="${TRACE_TOR_QUEUE:-1}"
TRACE_GOODPUT="${TRACE_GOODPUT:-1}"

if [[ -z "${TRACE_DIR+x}" ]]; then
  TS="$(date +%Y%m%d_%H%M%S)"
  DAY="$(date +%Y%m%d)"
  TRACE_DIR="$ROOT_DIR/outputs/sird-scenarios/${DAY}/HomaL4Protocol-good3-core-congestion_${PROFILE}_${TS}"
fi
PLOT_OUT_DIR="${PLOT_OUT_DIR:-$TRACE_DIR/plots}"
mkdir -p "$TRACE_DIR" "$PLOT_OUT_DIR"

export LD_LIBRARY_PATH="$ROOT_DIR/build/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

BIN_PATH="$ROOT_DIR/build/scratch/good3"
if [[ "$BUILD" == "1" || ! -x "$BIN_PATH" ]]; then
  echo "Building scratch/good3..."
  ./waf build
fi

run_case() {
  local tag="$1"
  local qdisc_mark_threshold="$2"
  local ecn_md="$3"
  local ecn_ai="$4"
  local log_file="$TRACE_DIR/good3_${tag}.run.log"
  local args=(
    "--simTag=$tag"
    "--outputDir=$TRACE_DIR"
    "--trafficConfig=core"
    "--workloadFile=$WORKLOAD_FILE"
    "--enableSird=1"
    "--offeredLoad=$OFFERED_LOAD"
    "--startSec=$START_SEC"
    "--durationSec=$DURATION_SEC"
    "--settleTailSec=$SETTLE_TAIL_SEC"
    "--traceMsg=$TRACE_MSG"
    "--traceTorQueue=$TRACE_TOR_QUEUE"
    "--traceGoodput=$TRACE_GOODPUT"
    "--queueSampleUs=$QUEUE_SAMPLE_US"
    "--goodputSampleUs=$GOODPUT_SAMPLE_US"
    "--deviceQueueMaxSize=$DEVICE_QUEUE_MAX_SIZE"
    "--qdiscMaxSize=$QDISC_MAX_SIZE"
    "--coreRateGbps=$CORE_RATE_GBPS"
    "--sirdEcnMdFactor=$ecn_md"
    "--sirdEcnAiStep=$ecn_ai"
  )
  if [[ -n "$qdisc_mark_threshold" ]]; then
    args+=("--qdiscMarkThreshold=$qdisc_mark_threshold")
  fi

  rm -f \
    "$TRACE_DIR/sim1_${tag}.goodput.tr" \
    "$TRACE_DIR/sim1_${tag}.tor-egress-queue.tr"

  echo "[$tag] start $(date '+%F %T')" | tee "$log_file"
  "$BIN_PATH" "${args[@]}" >>"$log_file" 2>&1
  echo "[$tag] done $(date '+%F %T')" | tee -a "$log_file"
}

run_case "control" "$QDISC_MARK_THRESHOLD" 0.75 1.0
run_case "no_ecn" "100000p" 1.0 0.0

python3 scripts/good3_analyze.py \
  --root "$TRACE_DIR" \
  --out-dir "$PLOT_OUT_DIR" \
  --start-sec "$START_SEC"

echo "good3 finished. report: $PLOT_OUT_DIR/good3_report_zh.md"
