#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

BUILD="${BUILD:-0}"
PLOT="${PLOT:-1}"
SIM_JOBS="${SIM_JOBS:-8}"

START_SEC="${START_SEC:-0.2}"
DURATION_SEC="${DURATION_SEC:-0.01}"
SETTLE_TAIL_SEC="${SETTLE_TAIL_SEC:-0.02}"
LINK_DELAY_US="${LINK_DELAY_US:-1}"
BDP_PKTS="${BDP_PKTS:-33.32}"

SHORT_MSG_SIZE_BYTES="${SHORT_MSG_SIZE_BYTES:-8192}"
SHORT_START_SPREAD_US="${SHORT_START_SPREAD_US:-0}"
TARGET_SHORT_MESSAGES_PER_SENDER="${TARGET_SHORT_MESSAGES_PER_SENDER:-0}"
ENABLE_LONG_FLOW="${ENABLE_LONG_FLOW:-1}"
LONG_MSG_SIZE_BYTES="${LONG_MSG_SIZE_BYTES:-10000000}"
LONG_SENDER_RATE_GBPS="${LONG_SENDER_RATE_GBPS:-17.0}"

DEVICE_QUEUE_MAX_SIZE="${DEVICE_QUEUE_MAX_SIZE:-1000p}"
QDISC_MAX_SIZE="${QDISC_MAX_SIZE:-1000p}"
SWITCH_QUEUE_SAMPLE_US="${SWITCH_QUEUE_SAMPLE_US:-1}"
LINK_THROUGHPUT_SAMPLE_US="${LINK_THROUGHPUT_SAMPLE_US:-100}"

SENDERS_RAW="${SENDERS:-32}"
LOADS_GBPS_RAW="${LOADS_GBPS:-60 90 110 130 160}"
MODES_RAW="${MODES:-unsched scheduled}"
read -r -a SENDERS_ARR <<< "$SENDERS_RAW"
read -r -a LOADS_ARR <<< "$LOADS_GBPS_RAW"
read -r -a MODES_ARR <<< "$MODES_RAW"

if [[ -z "${OUT_ROOT+x}" ]]; then
  TS="$(date +%Y%m%d_%H%M%S)"
  DAY="$(date +%Y%m%d)"
  OUT_ROOT="/mnt/nasDisk_ds3617/sird/unsched_storm/${DAY}/unsched_storm_delay1us_${TS}"
fi
if [[ "$OUT_ROOT" == *" "* ]]; then
  echo "OUT_ROOT must not contain spaces: $OUT_ROOT"
  exit 2
fi
mkdir -p "$OUT_ROOT"

export LD_LIBRARY_PATH="$ROOT_DIR/build/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

BIN_PATH="$ROOT_DIR/build/scratch/unsched_storm"
if [[ "$BUILD" == "1" || ! -x "$BIN_PATH" ]]; then
  ./waf build
fi

cat > "$OUT_ROOT/metadata.txt" <<META
experiment=unsched_storm
created_at=$(date '+%F %T')
root_dir=$ROOT_DIR
start_sec=$START_SEC
duration_sec=$DURATION_SEC
settle_tail_sec=$SETTLE_TAIL_SEC
link_delay_us=$LINK_DELAY_US
bdp_pkts=$BDP_PKTS
short_msg_size_bytes=$SHORT_MSG_SIZE_BYTES
senders=$SENDERS_RAW
loads_gbps=$LOADS_GBPS_RAW
modes=$MODES_RAW
short_start_spread_us=$SHORT_START_SPREAD_US
target_short_messages_per_sender=$TARGET_SHORT_MESSAGES_PER_SENDER
enable_long_flow=$ENABLE_LONG_FLOW
long_msg_size_bytes=$LONG_MSG_SIZE_BYTES
long_sender_rate_gbps=$LONG_SENDER_RATE_GBPS
device_queue_max_size=$DEVICE_QUEUE_MAX_SIZE
qdisc_max_size=$QDISC_MAX_SIZE
switch_queue_sample_us=$SWITCH_QUEUE_SAMPLE_US
link_throughput_sample_us=$LINK_THROUGHPUT_SAMPLE_US
META

COMMON_ARGS=(
  "--enableSird=1"
  "--startSec=$START_SEC"
  "--durationSec=$DURATION_SEC"
  "--settleTailSec=$SETTLE_TAIL_SEC"
  "--linkDelayUs=$LINK_DELAY_US"
  "--bdpPkts=$BDP_PKTS"
  "--shortMsgSizeBytes=$SHORT_MSG_SIZE_BYTES"
  "--shortStartSpreadUs=$SHORT_START_SPREAD_US"
  "--targetShortMessagesPerSender=$TARGET_SHORT_MESSAGES_PER_SENDER"
  "--enableLongFlow=$ENABLE_LONG_FLOW"
  "--longMsgSizeBytes=$LONG_MSG_SIZE_BYTES"
  "--longSenderRateGbps=$LONG_SENDER_RATE_GBPS"
  "--deviceQueueMaxSize=$DEVICE_QUEUE_MAX_SIZE"
  "--qdiscMaxSize=$QDISC_MAX_SIZE"
  "--traceMsg=1"
  "--traceSwitchEgressQueue=1"
  "--traceQueueDrop=1"
  "--traceQueueMark=1"
  "--traceLinkThroughput=1"
  "--switchQueueSampleUs=$SWITCH_QUEUE_SAMPLE_US"
  "--linkThroughputSampleUs=$LINK_THROUGHPUT_SAMPLE_US"
  "--showProgress=0"
)

mode_threshold() {
  local mode="$1"
  case "$mode" in
    unsched)
      echo 0
      ;;
    scheduled|scheduledish|scheduled-ish)
      echo 1
      ;;
    *)
      echo "Unknown mode: $mode" >&2
      return 2
      ;;
  esac
}

run_case() {
  local senders="$1"
  local load="$2"
  local mode="$3"
  local threshold
  threshold="$(mode_threshold "$mode")"

  local load_label="${load//./p}"
  local out_dir="$OUT_ROOT/n${senders}_${mode}_load${load_label}"
  local tag="n${senders}_${mode}_load${load_label}"
  local log_file="$out_dir/run.log"
  mkdir -p "$out_dir"

  echo "[$tag] start $(date '+%F %T')" > "$log_file"
  "$BIN_PATH" \
    "--simTag=$tag" \
    "--outputDir=$out_dir" \
    "--nShortSenders=$senders" \
    "--shortAggregateLoadGbps=$load" \
    "--sirdUnschThresholdPkts=$threshold" \
    "${COMMON_ARGS[@]}" >> "$log_file" 2>&1
  echo "[$tag] done $(date '+%F %T')" | tee -a "$log_file"
}

echo "OUT_ROOT=$OUT_ROOT"
echo "senders=$SENDERS_RAW"
echo "loads_gbps=$LOADS_GBPS_RAW"
echo "modes=$MODES_RAW"

pids=()
status=0
launch_case() {
  run_case "$@" &
  pids+=("$!")
  if [[ "${#pids[@]}" -ge "$SIM_JOBS" ]]; then
    if ! wait "${pids[0]}"; then
      status=1
    fi
    pids=("${pids[@]:1}")
  fi
}

for senders in "${SENDERS_ARR[@]}"; do
  for load in "${LOADS_ARR[@]}"; do
    for mode in "${MODES_ARR[@]}"; do
      launch_case "$senders" "$load" "$mode"
    done
  done
done

for pid in "${pids[@]}"; do
  if ! wait "$pid"; then
    status=1
  fi
done

if [[ "$status" -ne 0 ]]; then
  echo "At least one unsched_storm run failed. Check $OUT_ROOT/*/run.log"
  exit "$status"
fi

if [[ "$PLOT" == "1" ]]; then
  MPLCONFIGDIR="${MPLCONFIGDIR:-/tmp/unsched-storm-mpl}" \
  MPLBACKEND="${MPLBACKEND:-Agg}" \
  XDG_CACHE_HOME="${XDG_CACHE_HOME:-/tmp}" \
  /usr/bin/python3 scripts/unsched_storm_plot.py \
    --root "$OUT_ROOT" \
    --out-dir "$OUT_ROOT/plots" \
    --short-size "$SHORT_MSG_SIZE_BYTES" \
    --long-size "$LONG_MSG_SIZE_BYTES"
fi

echo "done OUT_ROOT=$OUT_ROOT"
