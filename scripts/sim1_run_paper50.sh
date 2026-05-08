#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

if [[ -z "${TRACE_ROOT+x}" ]]; then
  TS="$(date +%Y%m%d_%H%M%S)"
  DAY="$(date +%Y%m%d)"
  TRACE_ROOT="/mnt/nasDisk_ds3617/sird/sim1/${DAY}/sim1_paper50_original_style_${TS}"
fi

BUILD="${BUILD:-1}"
OFFERED_LOAD="${OFFERED_LOAD:-0.5}"
START_AT="${START_AT:-10.0}"
TRACE_LAST_RATIO="${TRACE_LAST_RATIO:-0.1}"
SETTLE_TAIL_SEC="${SETTLE_TAIL_SEC:-0.2}"
TRAFFIC_CONFIGS="${TRAFFIC_CONFIGS:-balanced core incast}"
TRACE_MSG="${TRACE_MSG:-1}"
TRACE_TOR_QUEUE="${TRACE_TOR_QUEUE:-1}"
TRACE_TOR_QUEUE_SERIES="${TRACE_TOR_QUEUE_SERIES:-0}"
TRACE_GOODPUT="${TRACE_GOODPUT:-1}"
QUEUE_SAMPLE_US="${QUEUE_SAMPLE_US:-100}"
GOODPUT_SAMPLE_US="${GOODPUT_SAMPLE_US:-100}"
DEVICE_QUEUE_MAX_SIZE="${DEVICE_QUEUE_MAX_SIZE:-2000p}"
QDISC_MAX_SIZE="${QDISC_MAX_SIZE:-1000p}"
QDISC_MARK_THRESHOLD="${QDISC_MARK_THRESHOLD:-}"
TOR_QUEUE_INCLUDE_DEVICE="${TOR_QUEUE_INCLUDE_DEVICE:-0}"

mkdir -p "$TRACE_ROOT"

for workload in google_rpc facebook_hadoop web_search; do
  phase_dir="$TRACE_ROOT/$workload"
  mkdir -p "$phase_dir"
  window_env="$phase_dir/window.env"
  window_json="$phase_dir/window.json"
  python3 scripts/sim1_paper_window.py \
    --workload "$workload" \
    --offered-load "$OFFERED_LOAD" \
    --start-at "$START_AT" \
    --trace-last-ratio "$TRACE_LAST_RATIO" \
    --json-out "$window_json" \
    --shell-out "$window_env"

  # shellcheck disable=SC1090
  source "$window_env"

  env \
    TRACE_DIR="$phase_dir" \
    BUILD="$BUILD" \
    WORKLOAD_TAGS="$workload" \
    TRAFFIC_CONFIGS="$TRAFFIC_CONFIGS" \
    OFFERED_LOAD="$OFFERED_LOAD" \
    START_SEC="$START_AT" \
    DURATION_SEC="$TRAFFIC_DURATION_SEC" \
    TRAFFIC_START_SEC="$TRAFFIC_START_SEC" \
    TRAFFIC_DURATION_SEC="$TRAFFIC_DURATION_SEC" \
    TRACE_START_SEC="$TRACE_START_SEC" \
    TRACE_DURATION_SEC="$TRACE_DURATION_SEC" \
    ANALYZE_START_SEC="$ANALYZE_START_SEC" \
    ANALYZE_DURATION_SEC="$ANALYZE_DURATION_SEC" \
    SETTLE_TAIL_SEC="$SETTLE_TAIL_SEC" \
    TRACE_MSG="$TRACE_MSG" \
    TRACE_TOR_QUEUE="$TRACE_TOR_QUEUE" \
    TRACE_TOR_QUEUE_SERIES="$TRACE_TOR_QUEUE_SERIES" \
    TRACE_GOODPUT="$TRACE_GOODPUT" \
    QUEUE_SAMPLE_US="$QUEUE_SAMPLE_US" \
    GOODPUT_SAMPLE_US="$GOODPUT_SAMPLE_US" \
    DEVICE_QUEUE_MAX_SIZE="$DEVICE_QUEUE_MAX_SIZE" \
    QDISC_MAX_SIZE="$QDISC_MAX_SIZE" \
    QDISC_MARK_THRESHOLD="$QDISC_MARK_THRESHOLD" \
    TOR_QUEUE_INCLUDE_DEVICE="$TOR_QUEUE_INCLUDE_DEVICE" \
    bash scripts/sim1_run_two.sh

  BUILD=0
done

echo "[sim1_paper50] complete root=$TRACE_ROOT"
