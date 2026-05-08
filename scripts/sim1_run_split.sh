#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

if [[ -z "${TRACE_ROOT+x}" ]]; then
  TS="$(date +%Y%m%d_%H%M%S)"
  DAY="$(date +%Y%m%d)"
  TRACE_ROOT="/mnt/nasDisk_ds3617/sird/sim1/${DAY}/sim1_split_${TS}"
fi

BUILD="${BUILD:-1}"
OFFERED_LOAD="${OFFERED_LOAD:-0.5}"
START_SEC="${START_SEC:-0.2}"
DURATION_SEC="${DURATION_SEC:-0.25}"
SETTLE_TAIL_SEC="${SETTLE_TAIL_SEC:-0.2}"
QUEUE_SAMPLE_US="${QUEUE_SAMPLE_US:-100}"
GOODPUT_SAMPLE_US="${GOODPUT_SAMPLE_US:-100}"
TRAFFIC_CONFIGS="${TRAFFIC_CONFIGS:-balanced core incast}"
DEVICE_QUEUE_MAX_SIZE="${DEVICE_QUEUE_MAX_SIZE:-2000p}"
QDISC_MAX_SIZE="${QDISC_MAX_SIZE:-1000p}"
QDISC_MARK_THRESHOLD="${QDISC_MARK_THRESHOLD:-}"
MAX_SETTLE_RETRIES="${MAX_SETTLE_RETRIES:-3}"
SETTLE_TAIL_MULTIPLIER="${SETTLE_TAIL_MULTIPLIER:-2}"

mkdir -p "$TRACE_ROOT"

phase_run() {
  local phase_name="$1"
  local trace_msg="$2"
  local trace_tor_queue="$3"
  local trace_tor_queue_series="$4"
  local trace_goodput="$5"
  local phase_dir="$TRACE_ROOT/$phase_name"

  echo "[sim1_split] phase=$phase_name dir=$phase_dir"
  mkdir -p "$phase_dir"

  env \
    TRACE_DIR="$phase_dir" \
    BUILD="$BUILD" \
    OFFERED_LOAD="$OFFERED_LOAD" \
    START_SEC="$START_SEC" \
    DURATION_SEC="$DURATION_SEC" \
    SETTLE_TAIL_SEC="$SETTLE_TAIL_SEC" \
    QUEUE_SAMPLE_US="$QUEUE_SAMPLE_US" \
    GOODPUT_SAMPLE_US="$GOODPUT_SAMPLE_US" \
    TRAFFIC_CONFIGS="$TRAFFIC_CONFIGS" \
    TRACE_MSG="$trace_msg" \
    TRACE_TOR_QUEUE="$trace_tor_queue" \
    TRACE_TOR_QUEUE_SERIES="$trace_tor_queue_series" \
    TRACE_GOODPUT="$trace_goodput" \
    DEVICE_QUEUE_MAX_SIZE="$DEVICE_QUEUE_MAX_SIZE" \
    QDISC_MAX_SIZE="$QDISC_MAX_SIZE" \
    QDISC_MARK_THRESHOLD="$QDISC_MARK_THRESHOLD" \
    MAX_SETTLE_RETRIES="$MAX_SETTLE_RETRIES" \
    SETTLE_TAIL_MULTIPLIER="$SETTLE_TAIL_MULTIPLIER" \
    bash scripts/sim1_run_two.sh
}

phase_run "goodput_queue" 0 1 0 1
BUILD=0
phase_run "slowdown" 1 0 0 0

echo "[sim1_split] complete root=$TRACE_ROOT"
