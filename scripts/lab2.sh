#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

PROFILE="${1:-fast}"

START_SEC="${START_SEC:-0.2}"
DURATION_SEC="${DURATION_SEC:-0.6}"
FLOW_GAP_US="${FLOW_GAP_US:-200000}"

if [[ "$PROFILE" == "full" ]]; then
  START_SEC="${START_SEC_FULL:-0.2}"
  DURATION_SEC="${DURATION_SEC_FULL:-13.2}"
  FLOW_GAP_US="${FLOW_GAP_US_FULL:-4500000}"
elif [[ "$PROFILE" != "fast" ]]; then
  echo "Usage: bash scripts/lab2.sh [fast|full]"
  exit 2
fi

BUILD="${BUILD:-1}"
PLOT="${PLOT:-1}"
OUTCAST_MSG_SIZE_BYTES="${OUTCAST_MSG_SIZE_BYTES:-10000000}"
TRACE_SIRD_CREDIT="${TRACE_SIRD_CREDIT:-0}"
TRACE_SIRD_BUCKET="${TRACE_SIRD_BUCKET:-0}"
TRACE_CREDIT_EVENTS="${TRACE_CREDIT_EVENTS:-0}"
TRACE_CREDIT_SERIES="${TRACE_CREDIT_SERIES:-1}"
TRACE_SWITCH_QUEUE="${TRACE_SWITCH_QUEUE:-1}"
TRACE_SWITCH_QUEUE_SAMPLE_US="${TRACE_SWITCH_QUEUE_SAMPLE_US:-1000}"

# Paper-aligned defaults for Section 6.1:
# BDP=24 pkts, B=1.5*BDP=36 pkts, UnschT=1*BDP=24 pkts, SThr=0.5*BDP=12 pkts.
RTT_PKTS="${RTT_PKTS:-24}"
SIRD_CREDIT_BUDGET_PKTS="${SIRD_CREDIT_BUDGET_PKTS:-36}"
SIRD_UNSCH_THRESHOLD_PKTS="${SIRD_UNSCH_THRESHOLD_PKTS:-24}"
SIRD_CSN_THRESHOLD_PKTS="${SIRD_CSN_THRESHOLD_PKTS:-12}"
SIRD_INF_CSN_THRESHOLD_PKTS="${SIRD_INF_CSN_THRESHOLD_PKTS:-65535}"
DEVICE_QUEUE_MAX_SIZE="${DEVICE_QUEUE_MAX_SIZE:-2000p}"
QDISC_MAX_SIZE="${QDISC_MAX_SIZE:-1000p}"
QDISC_MARK_THRESHOLD="${QDISC_MARK_THRESHOLD:-0p}"

TRACE_DIR="${TRACE_DIR:-$ROOT_DIR/outputs/sird-scenarios/HomaL4Protocol-lab2-sender-congestion}"
PLOT_OUT_DIR="${PLOT_OUT_DIR:-$ROOT_DIR/output-f/lab2}"
mkdir -p "$TRACE_DIR"

export LD_LIBRARY_PATH="$ROOT_DIR/build/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

BIN_PATH="$ROOT_DIR/build/scratch/lab2"
if [[ "$BUILD" == "1" || ! -x "$BIN_PATH" ]]; then
  echo "Building scratch/lab2..."
  ./waf build
fi

COMMON_ARGS=(
  "--enableSird=1"
  "--outputDir=$TRACE_DIR"
  "--traceMsg=1"
  "--traceSirdCredit=$TRACE_SIRD_CREDIT"
  "--traceSirdBucket=$TRACE_SIRD_BUCKET"
  "--traceCreditEvents=$TRACE_CREDIT_EVENTS"
  "--traceCreditSeries=$TRACE_CREDIT_SERIES"
  "--traceSwitchEgressQueue=$TRACE_SWITCH_QUEUE"
  "--creditSampleUs=1000"
  "--switchQueueSampleUs=$TRACE_SWITCH_QUEUE_SAMPLE_US"
  "--rttPkts=$RTT_PKTS"
  "--sirdCreditBudgetPkts=$SIRD_CREDIT_BUDGET_PKTS"
  "--sirdUnschThresholdPkts=$SIRD_UNSCH_THRESHOLD_PKTS"
  "--startSec=$START_SEC"
  "--durationSec=$DURATION_SEC"
  "--settleTailSec=0.1"
  "--msgSizeBytes=$OUTCAST_MSG_SIZE_BYTES"
  "--flowGapUs=$FLOW_GAP_US"
  "--deviceQueueMaxSize=$DEVICE_QUEUE_MAX_SIZE"
  "--qdiscMaxSize=$QDISC_MAX_SIZE"
  "--qdiscMarkThreshold=$QDISC_MARK_THRESHOLD"
)

run_case() {
  local tag="$1"
  local csn_threshold="$2"
  local sender_md="$3"
  local sender_ai="$4"
  local log_file="$TRACE_DIR/lab2_${tag}.run.log"

  rm -f \
    "$TRACE_DIR/lab2_${tag}.msg.tr" \
    "$TRACE_DIR/lab2_${tag}.sird-credit.tr" \
    "$TRACE_DIR/lab2_${tag}.sird-bucket.tr" \
    "$TRACE_DIR/lab2_${tag}.credit-events.tr" \
    "$TRACE_DIR/lab2_${tag}.credit-series.tr" \
    "$TRACE_DIR/lab2_${tag}.switch-egress-queue.tr"

  echo "[$tag] start $(date '+%F %T')" | tee "$log_file"
  "$BIN_PATH" \
    "--simTag=$tag" \
    "--sirdSenderCsnThresholdPkts=$csn_threshold" \
    "--sirdSenderMdFactor=$sender_md" \
    "--sirdSenderAiStep=$sender_ai" \
    "${COMMON_ARGS[@]}" >>"$log_file" 2>&1
  echo "[$tag] done $(date '+%F %T')" | tee -a "$log_file"
}

echo "profile=$PROFILE durationSec=$DURATION_SEC flowGapUs=$FLOW_GAP_US"
echo "outputs: $TRACE_DIR"

pids=()
run_case "feedback" "$SIRD_CSN_THRESHOLD_PKTS" 0.8 1.0 &
pids+=("$!")
run_case "no_feedback" "$SIRD_INF_CSN_THRESHOLD_PKTS" 1.0 0.0 &
pids+=("$!")

status=0
for pid in "${pids[@]}"; do
  if ! wait "$pid"; then
    status=1
  fi
done

if [[ "$status" -ne 0 ]]; then
  echo "At least one lab2 run failed. Check $TRACE_DIR/lab2_*.run.log"
  exit "$status"
fi

echo "lab2 runs finished."

if [[ "$PLOT" == "1" ]]; then
  mkdir -p "$PLOT_OUT_DIR"
  rm -f "$PLOT_OUT_DIR"/lab2_*.png "$PLOT_OUT_DIR"/lab2_summary.csv
  python3 scripts/lab2_plot.py \
    --trace-dir "$TRACE_DIR" \
    --out-dir "$PLOT_OUT_DIR" \
    --feedback-tag feedback \
    --no-feedback-tag no_feedback \
    --bdp-pkts "$RTT_PKTS" \
    --budget-pkts "$SIRD_CREDIT_BUDGET_PKTS"
fi
