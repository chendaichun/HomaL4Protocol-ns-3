#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

START_SEC="${START_SEC:-0.2}"
SETTLE_TAIL_SEC="${SETTLE_TAIL_SEC:-0.3}"
SHORT_INTERVAL_8B_US="${SHORT_INTERVAL_8B_US:-1}"
SHORT_INTERVAL_500KB_US="${SHORT_INTERVAL_500KB_US:-1000}"
LONG_MSG_SIZE_BYTES="${LONG_MSG_SIZE_BYTES:-10000000}"
LONG_SENDER_RATE_GBPS="${LONG_SENDER_RATE_GBPS:-16.67}"
BDP_PKTS="${BDP_PKTS:-150}"
DEVICE_QUEUE_MAX_SIZE="${DEVICE_QUEUE_MAX_SIZE:-1000p}"
QDISC_MAX_SIZE="${QDISC_MAX_SIZE:-1000p}"
REPLY_SIZE_BYTES="${REPLY_SIZE_BYTES:-8}"
BUILD="${BUILD:-0}"
PLOT="${PLOT:-1}"
PLOT_JOBS="${PLOT_JOBS:-8}"

PROBE_COUNTS_RAW="${PROBE_COUNTS:-40 80 120 160 200 240 280 320 360 400}"
read -r -a PROBE_COUNTS_ARR <<< "$PROBE_COUNTS_RAW"

if [[ -z "${OUT_ROOT+x}" ]]; then
  TS="$(date +%Y%m%d_%H%M%S)"
  OUT_ROOT="/mnt/nasDisk_ds3617/sird/lab1_o/lab1_o_sweep_rr_delay4p5us_bdp150_${TS}"
fi
if [[ "$OUT_ROOT" == *" "* ]]; then
  echo "OUT_ROOT must not contain spaces: $OUT_ROOT"
  exit 2
fi
mkdir -p "$OUT_ROOT"

export LD_LIBRARY_PATH="$ROOT_DIR/build/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

BIN_PATH="$ROOT_DIR/build/scratch/lab1_o"
if [[ "$BUILD" == "1" || ! -x "$BIN_PATH" ]]; then
  ./waf build
fi

COMMON_ARGS=(
  "--enableSird=1"
  "--startSec=$START_SEC"
  "--longMsgSizeBytes=$LONG_MSG_SIZE_BYTES"
  "--longSenderRateGbps=$LONG_SENDER_RATE_GBPS"
  "--bdpPkts=$BDP_PKTS"
  "--traceMsg=1"
  "--traceRequestReplyLatency=1"
  "--replySizeBytes=$REPLY_SIZE_BYTES"
  "--tracePathRtt=0"
  "--traceSwitchEgressQueue=0"
  "--traceLinkThroughput=0"
  "--traceSirdCredit=0"
  "--traceSirdLoop=0"
  "--deviceQueueMaxSize=$DEVICE_QUEUE_MAX_SIZE"
  "--qdiscMaxSize=$QDISC_MAX_SIZE"
  "--showProgressBar=0"
)

run_case() {
  local probe_count="$1"
  local label="$2"
  local probe_size="$3"
  local enable_background="$4"
  local use_srr="$5"
  local interval_us="$6"
  local out_dir="$OUT_ROOT/probe${probe_count}"
  local tag="lab1_fast_sird_${label}"
  local duration_sec
  duration_sec=$(awk -v n="$probe_count" -v us="$interval_us" 'BEGIN {printf "%.9f", n * us / 1000000.0}')

  mkdir -p "$out_dir"
  local log_file="$out_dir/${tag}.run.log"
  local msg_trace="$out_dir/lab1_${tag}.msg.tr"
  rm -f "$msg_trace"

  echo "[$tag probe=$probe_count] start durationSec=$duration_sec intervalUs=$interval_us $(date '+%F %T')" > "$log_file"
  "$BIN_PATH" \
    "--simTag=$tag" \
    "--outputDir=$out_dir" \
    "--shortMsgSizeBytes=$probe_size" \
    "--shortIntervalUs=$interval_us" \
    "--durationSec=$duration_sec" \
    "--targetProbeMessages=$probe_count" \
    "--enableBackgroundTraffic=$enable_background" \
    "--useSrrScheduling=$use_srr" \
    "--settleTailSec=$SETTLE_TAIL_SEC" \
    "${COMMON_ARGS[@]}" >> "$log_file" 2>&1

  local started finished
  started=$(awk -v sz="$probe_size" '$1=="+" && $3==sz {c++} END {print c+0}' "$msg_trace")
  finished=$(awk -v sz="$probe_size" '$1=="-" && $3==sz {c++} END {print c+0}' "$msg_trace")
  echo "[$tag probe=$probe_count] done started=$started finished=$finished $(date '+%F %T')" | tee -a "$log_file"
  test "$started" -eq "$probe_count"
  test "$finished" -eq "$probe_count"
}

echo "OUT_ROOT=$OUT_ROOT"
echo "probe_counts=$PROBE_COUNTS_RAW"
echo "starting $((${#PROBE_COUNTS_ARR[@]} * 5)) simulation processes"

pids=()
for probe_count in "${PROBE_COUNTS_ARR[@]}"; do
  run_case "$probe_count" "unloaded_8B" 8 0 0 "$SHORT_INTERVAL_8B_US" &
  pids+=("$!")
  run_case "$probe_count" "incast_8B_srpt" 8 1 0 "$SHORT_INTERVAL_8B_US" &
  pids+=("$!")
  run_case "$probe_count" "unloaded_500KB" 500000 0 0 "$SHORT_INTERVAL_500KB_US" &
  pids+=("$!")
  run_case "$probe_count" "incast_500KB_srpt" 500000 1 0 "$SHORT_INTERVAL_500KB_US" &
  pids+=("$!")
  run_case "$probe_count" "incast_500KB_srr" 500000 1 1 "$SHORT_INTERVAL_500KB_US" &
  pids+=("$!")
done

status=0
for pid in "${pids[@]}"; do
  if ! wait "$pid"; then
    status=1
  fi
done

if [[ "$status" -ne 0 ]]; then
  echo "At least one lab1_o sweep run failed. Check $OUT_ROOT/probe*/**.run.log"
  exit "$status"
fi

echo "all simulations finished"

if [[ "$PLOT" == "1" ]]; then
  plot_one() {
    local probe_count="$1"
    local out_dir="$OUT_ROOT/probe${probe_count}"
    local plot_dir="$out_dir/plots"
    mkdir -p "$plot_dir"
    MPLCONFIGDIR="${MPLCONFIGDIR:-/tmp/lab1o-sweep-mpl}" \
    XDG_CACHE_HOME="${XDG_CACHE_HOME:-/tmp}" \
    /usr/bin/python3 scripts/lab1_plot.py \
      --trace-dir "$out_dir" \
      --out-dir "$plot_dir" \
      --tag lab1_fast_sird_unloaded_8B \
      --tag lab1_fast_sird_incast_8B_srpt \
      --tag lab1_fast_sird_unloaded_500KB \
      --tag lab1_fast_sird_incast_500KB_srpt \
      --tag lab1_fast_sird_incast_500KB_srr \
      --size 8 \
      --size 500000 \
      --start-sec "$START_SEC" > "$out_dir/plot.log" 2>&1
    echo "plotted probe=$probe_count"
  }

  plot_pids=()
  for probe_count in "${PROBE_COUNTS_ARR[@]}"; do
    plot_one "$probe_count" &
    plot_pids+=("$!")
    if [[ "${#plot_pids[@]}" -ge "$PLOT_JOBS" ]]; then
      wait "${plot_pids[0]}"
      plot_pids=("${plot_pids[@]:1}")
    fi
  done
  for pid in "${plot_pids[@]}"; do
    wait "$pid"
  done

  summary="$OUT_ROOT/lab1_o_sweep_summary.csv"
  first=1
  for probe_count in "${PROBE_COUNTS_ARR[@]}"; do
    csv="$OUT_ROOT/probe${probe_count}/plots/lab1_summary.csv"
    if [[ "$first" == "1" ]]; then
      awk -v pc="$probe_count" 'NR==1 {print "probe_count," $0} NR>1 {print pc "," $0}' "$csv" > "$summary"
      first=0
    else
      awk -v pc="$probe_count" 'NR>1 {print pc "," $0}' "$csv" >> "$summary"
    fi
  done
  echo "SUMMARY=$summary"
fi

echo "OUT_ROOT=$OUT_ROOT"
