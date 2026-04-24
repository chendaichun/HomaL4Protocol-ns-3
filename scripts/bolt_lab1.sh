#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

PROFILE="${1:-fast}"

START_SEC="${START_SEC:-0.2}"
DURATION_SEC="${DURATION_SEC:-0.005}"
SETTLE_TAIL_SEC="${SETTLE_TAIL_SEC:-0.08}"
SHORT_INTERVAL_8B_US="${SHORT_INTERVAL_8B_US:-1}"
SHORT_INTERVAL_500KB_US="${SHORT_INTERVAL_500KB_US:-1000}"
TARGET_PROBE_MESSAGES="${TARGET_PROBE_MESSAGES:-40}"

case "$PROFILE" in
  fast)
    ;;
  smoke)
    TARGET_PROBE_MESSAGES="${TARGET_PROBE_MESSAGES_SMOKE:-10}"
    SETTLE_TAIL_SEC="${SETTLE_TAIL_SEC_SMOKE:-0.05}"
    ;;
  *)
    echo "Usage: bash scripts/bolt_lab1.sh [smoke|fast]"
    exit 2
    ;;
esac

LONG_MSG_SIZE_BYTES="${LONG_MSG_SIZE_BYTES:-10000000}"
LONG_SENDER_RATE_GBPS="${LONG_SENDER_RATE_GBPS:-16.67}"
PLOT="${PLOT:-1}"
BUILD="${BUILD:-1}"
ENFORCE_MSG_COMPLETE="${ENFORCE_MSG_COMPLETE:-0}"
MAX_SETTLE_RETRIES="${MAX_SETTLE_RETRIES:-3}"
SETTLE_TAIL_MULTIPLIER="${SETTLE_TAIL_MULTIPLIER:-2}"
DEVICE_QUEUE_MAX_SIZE="${DEVICE_QUEUE_MAX_SIZE:-1000p}"
QDISC_MAX_SIZE="${QDISC_MAX_SIZE:-1000p}"

if [[ -z "${OUT_DIR+x}" ]]; then
  OUT_DIR="outputs/sird-scenarios/bolt-lab1"
fi
if [[ "$OUT_DIR" == *" "* ]]; then
  echo "OUT_DIR must not contain spaces because ns-3 trace output parsing truncates such paths: $OUT_DIR"
  exit 2
fi
PLOT_OUT_DIR="${PLOT_OUT_DIR:-$OUT_DIR/plots}"
mkdir -p "$OUT_DIR"

export LD_LIBRARY_PATH="$ROOT_DIR/build/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

BIN_PATH="$ROOT_DIR/build/scratch/bolt-lab1"
if [[ "$BUILD" == "1" || ! -x "$BIN_PATH" ]]; then
  echo "Building scratch/bolt-lab1..."
  ./waf build
fi

COMMON_ARGS=(
  "--outputDir=$OUT_DIR"
  "--startSec=$START_SEC"
  "--longMsgSizeBytes=$LONG_MSG_SIZE_BYTES"
  "--longSenderRateGbps=$LONG_SENDER_RATE_GBPS"
  "--traceMsg=1"
  "--deviceQueueMaxSize=$DEVICE_QUEUE_MAX_SIZE"
  "--qdiscMaxSize=$QDISC_MAX_SIZE"
)

run_case() {
  local cc_mode="$1"
  local label="$2"
  local probe_size="$3"
  local enable_background="$4"
  local short_interval_us="$5"
  local mode_label
  mode_label="$(echo "$cc_mode" | tr '[:upper:]' '[:lower:]')"
  local tag="bolt_lab1_${PROFILE}_${mode_label}_${label}"
  local log_file="$OUT_DIR/${tag}.run.log"
  local settle_tail_sec="$SETTLE_TAIL_SEC"
  local duration_sec="$DURATION_SEC"
  local attempt=1

  if [[ "$TARGET_PROBE_MESSAGES" -gt 0 ]]; then
    duration_sec=$(awk -v n="$TARGET_PROBE_MESSAGES" -v us="$short_interval_us" 'BEGIN {printf "%.9f", n * us / 1000000.0}')
  fi

  while true; do
    rm -f "$OUT_DIR/lab1_${tag}.msg.tr"

    echo "[$tag] attempt=$attempt ccMode=$cc_mode durationSec=$duration_sec settleTailSec=$settle_tail_sec shortIntervalUs=$short_interval_us targetProbeMessages=$TARGET_PROBE_MESSAGES start $(date '+%F %T')" | tee "$log_file"
    "$BIN_PATH" \
      "--simTag=$tag" \
      "--ccMode=$cc_mode" \
      "--shortMsgSizeBytes=$probe_size" \
      "--shortIntervalUs=$short_interval_us" \
      "--durationSec=$duration_sec" \
      "--targetProbeMessages=$TARGET_PROBE_MESSAGES" \
      "--enableBackgroundTraffic=$enable_background" \
      "--settleTailSec=$settle_tail_sec" \
      "${COMMON_ARGS[@]}" >>"$log_file" 2>&1

    local msg_trace="$OUT_DIR/lab1_${tag}.msg.tr"
    local started=0
    local finished=0
    if [[ -f "$msg_trace" ]]; then
      started=$(awk -v sz="$probe_size" '$1=="+" && $3==sz {c++} END {print c+0}' "$msg_trace")
      finished=$(awk -v sz="$probe_size" '$1=="-" && $3==sz {c++} END {print c+0}' "$msg_trace")
    fi

    if [[ "$ENFORCE_MSG_COMPLETE" != "1" ]]; then
      echo "[$tag] done $(date '+%F %T') started=$started finished=$finished incomplete=$((started - finished))" | tee -a "$log_file"
      break
    fi

    if [[ "$started" -eq "$finished" && "$started" -gt 0 ]]; then
      echo "[$tag] done $(date '+%F %T') started=$started finished=$finished" | tee -a "$log_file"
      break
    fi

    if [[ "$attempt" -ge "$MAX_SETTLE_RETRIES" ]]; then
      echo "[$tag] incomplete after $attempt attempts: started=$started finished=$finished" | tee -a "$log_file"
      return 1
    fi

    echo "[$tag] incomplete: started=$started finished=$finished, increasing settleTailSec" | tee -a "$log_file"
    settle_tail_sec=$(awk -v x="$settle_tail_sec" -v m="$SETTLE_TAIL_MULTIPLIER" 'BEGIN {printf "%.6f", x*m}')
    attempt=$((attempt + 1))
  done
}

echo "profile=$PROFILE durationSec=$DURATION_SEC settleTailSec=$SETTLE_TAIL_SEC targetProbeMessages=$TARGET_PROBE_MESSAGES"
echo "outputs: $OUT_DIR"

pids=()
for cc_mode in DEFAULT SWIFT; do
  run_case "$cc_mode" "unloaded_8B" 8 0 "$SHORT_INTERVAL_8B_US" &
  pids+=("$!")
  run_case "$cc_mode" "incast_8B" 8 1 "$SHORT_INTERVAL_8B_US" &
  pids+=("$!")
  run_case "$cc_mode" "unloaded_500KB" 500000 0 "$SHORT_INTERVAL_500KB_US" &
  pids+=("$!")
  run_case "$cc_mode" "incast_500KB" 500000 1 "$SHORT_INTERVAL_500KB_US" &
  pids+=("$!")
done

status=0
for pid in "${pids[@]}"; do
  if ! wait "$pid"; then
    status=1
  fi
done

if [[ "$status" -ne 0 ]]; then
  echo "At least one bolt-lab1 run failed. Check $OUT_DIR/*.run.log"
  exit "$status"
fi

echo "bolt-lab1 runs finished."

if [[ "$PLOT" == "1" ]]; then
  mkdir -p "$PLOT_OUT_DIR"
  rm -f "$PLOT_OUT_DIR"/lab1_*.png "$PLOT_OUT_DIR"/lab1_summary.csv
  MPL_CACHE_DIR="${MPLCONFIGDIR:-/tmp/bolt-lab1-matplotlib-cache}"
  XDG_CACHE_DIR="${XDG_CACHE_HOME:-/tmp}"
  mkdir -p "$MPL_CACHE_DIR" "$XDG_CACHE_DIR"
  PLOT_PYTHON="${PYTHON:-}"
  if [[ -z "$PLOT_PYTHON" ]]; then
    for candidate in /opt/miniconda3/bin/python3 /usr/bin/python3 python3; do
      if command -v "$candidate" >/dev/null 2>&1 && \
         MPLCONFIGDIR="$MPL_CACHE_DIR" XDG_CACHE_HOME="$XDG_CACHE_DIR" "$candidate" -c 'import matplotlib' >/dev/null 2>&1; then
        PLOT_PYTHON="$(command -v "$candidate")"
        break
      fi
    done
  fi
  if [[ -z "$PLOT_PYTHON" ]]; then
    echo "No Python with matplotlib found. Set PYTHON=/path/to/python3 and rerun plotting."
    exit 1
  fi
  echo "plotting with $PLOT_PYTHON"
  MPLCONFIGDIR="$MPL_CACHE_DIR" XDG_CACHE_HOME="$XDG_CACHE_DIR" "$PLOT_PYTHON" scripts/lab1_plot.py \
    --trace-dir "$OUT_DIR" \
    --out-dir "$PLOT_OUT_DIR" \
    --tag "bolt_lab1_${PROFILE}_default_unloaded_8B" \
    --tag "bolt_lab1_${PROFILE}_default_incast_8B" \
    --tag "bolt_lab1_${PROFILE}_swift_unloaded_8B" \
    --tag "bolt_lab1_${PROFILE}_swift_incast_8B" \
    --tag "bolt_lab1_${PROFILE}_default_unloaded_500KB" \
    --tag "bolt_lab1_${PROFILE}_default_incast_500KB" \
    --tag "bolt_lab1_${PROFILE}_swift_unloaded_500KB" \
    --tag "bolt_lab1_${PROFILE}_swift_incast_500KB" \
    --size 8 \
    --size 500000 \
    --start-sec "$START_SEC"
fi
