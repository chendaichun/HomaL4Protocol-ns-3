#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

OUT_DIR="${OUT_DIR:-$ROOT_DIR/outputs/sird-scenarios/HomaL4Protocol-lab1-proof}"
BUILD="${BUILD:-1}"
START_SEC="${START_SEC:-0.2}"
DURATION_SEC="${DURATION_SEC:-0.3}"
SETTLE_TAIL_SEC="${SETTLE_TAIL_SEC:-2.4}"
SHORT_INTERVAL_US="${SHORT_INTERVAL_US:-200}"
LONG_MSG_SIZE_BYTES="${LONG_MSG_SIZE_BYTES:-10000000}"
LONG_SENDER_RATE_GBPS="${LONG_SENDER_RATE_GBPS:-17.0}"
DEVICE_QUEUE_MAX_SIZE="${DEVICE_QUEUE_MAX_SIZE:-1000p}"
QDISC_MARK_THRESHOLD="${QDISC_MARK_THRESHOLD:-43p}"

mkdir -p "$OUT_DIR"
export LD_LIBRARY_PATH="$ROOT_DIR/build/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

BIN_PATH="$ROOT_DIR/build/scratch/lab1"
if [[ "$BUILD" == "1" || ! -x "$BIN_PATH" ]]; then
  ./waf build
fi

run_one() {
  local tag="$1"
  local qdisc_max_size="$2"
  local log_file="$OUT_DIR/${tag}.run.log"

  rm -f \
    "$OUT_DIR/lab1_${tag}.msg.tr"

  echo "[$tag] start $(date '+%F %T') qdiscMaxSize=$qdisc_max_size" | tee "$log_file"
  "$BIN_PATH" \
    "--simTag=$tag" \
    "--outputDir=$OUT_DIR" \
    "--shortMsgSizeBytes=500000" \
    "--enableBackgroundTraffic=1" \
    "--useSrrScheduling=1" \
    "--enableSird=1" \
    "--startSec=$START_SEC" \
    "--durationSec=$DURATION_SEC" \
    "--settleTailSec=$SETTLE_TAIL_SEC" \
    "--longMsgSizeBytes=$LONG_MSG_SIZE_BYTES" \
    "--longSenderRateGbps=$LONG_SENDER_RATE_GBPS" \
    "--shortIntervalUs=$SHORT_INTERVAL_US" \
    "--rttPkts=34" \
    "--sirdCreditBudgetPkts=40" \
    "--sirdUnschThresholdPkts=40" \
    "--sirdSenderCsnThresholdPkts=17" \
    "--deviceQueueMaxSize=$DEVICE_QUEUE_MAX_SIZE" \
    "--qdiscMaxSize=$qdisc_max_size" \
    "--qdiscMarkThreshold=$QDISC_MARK_THRESHOLD" \
    "--traceMsg=1" \
    "--tracePathRtt=0" \
    "--traceSwitchEgressQueue=0" \
    "--traceLinkThroughput=0" \
    "--traceSirdCredit=0" \
    "--showProgressBar=0" >>"$log_file" 2>&1

  local msg_trace="$OUT_DIR/lab1_${tag}.msg.tr"
  local started finished
  started=$(awk '$1=="+" && $3==500000 {c++} END {print c+0}' "$msg_trace")
  finished=$(awk '$1=="-" && $3==500000 {c++} END {print c+0}' "$msg_trace")
  echo "[$tag] done $(date '+%F %T') started=$started finished=$finished" | tee -a "$log_file"
}

pids=()
run_one "proof_srr_128p" "128p" &
pids+=("$!")
run_one "proof_srr_4096p" "4096p" &
pids+=("$!")

status=0
for pid in "${pids[@]}"; do
  if ! wait "$pid"; then
    status=1
  fi
done

{
  echo "tag,qdisc_max_size,started,finished,incomplete"
  for tag in proof_srr_128p proof_srr_4096p; do
    msg_trace="$OUT_DIR/lab1_${tag}.msg.tr"
    started=$(awk '$1=="+" && $3==500000 {c++} END {print c+0}' "$msg_trace")
    finished=$(awk '$1=="-" && $3==500000 {c++} END {print c+0}' "$msg_trace")
    qdisc_max_size="128p"
    [[ "$tag" == "proof_srr_4096p" ]] && qdisc_max_size="4096p"
    echo "$tag,$qdisc_max_size,$started,$finished,$((started-finished))"
  done
} > "$OUT_DIR/lab1_proof_summary.csv"

exit "$status"
