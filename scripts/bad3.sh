#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

PROFILE="${1:-fast}"

START_SEC="${START_SEC:-0.2}"
DURATION_SEC="${DURATION_SEC:-4.2}"
FLOW_GAP_US="${FLOW_GAP_US:-1400000}"
ACTIVE_RECEIVER_COUNT="${ACTIVE_RECEIVER_COUNT:-3}"
SETTLE_TAIL_SEC="${SETTLE_TAIL_SEC:-0.1}"
BACKLOG_DEPTH_MSGS="${BACKLOG_DEPTH_MSGS:-4}"
MSG_SIZE_BYTES="${MSG_SIZE_BYTES:-300000}"

if [[ "$PROFILE" == "smoke" ]]; then
  DURATION_SEC="${DURATION_SEC_SMOKE:-1.8}"
  FLOW_GAP_US="${FLOW_GAP_US_SMOKE:-600000}"
  SETTLE_TAIL_SEC="${SETTLE_TAIL_SEC_SMOKE:-0.05}"
elif [[ "$PROFILE" == "full" ]]; then
  DURATION_SEC="${DURATION_SEC_FULL:-13.2}"
  FLOW_GAP_US="${FLOW_GAP_US_FULL:-4500000}"
  SETTLE_TAIL_SEC="${SETTLE_TAIL_SEC_FULL:-0.1}"
elif [[ "$PROFILE" != "fast" ]]; then
  echo "Usage: bash scripts/bad3.sh [smoke|fast|full]"
  exit 2
fi

BUILD="${BUILD:-1}"
ANALYZE="${ANALYZE:-1}"
SIM_JOBS="${SIM_JOBS:-2}"

TRACE_MSG="${TRACE_MSG:-0}"
TRACE_PROTOCOL_CREDIT="${TRACE_PROTOCOL_CREDIT:-0}"
TRACE_CREDIT_SAMPLE="${TRACE_CREDIT_SAMPLE:-1}"
TRACE_GOODPUT="${TRACE_GOODPUT:-1}"
TRACE_STHR_SAMPLE="${TRACE_STHR_SAMPLE:-1}"
TRACE_SIRD_CREDIT="${TRACE_SIRD_CREDIT:-0}"
TRACE_SIRD_BUCKET="${TRACE_SIRD_BUCKET:-0}"
TRACE_SWITCH_QUEUE="${TRACE_SWITCH_QUEUE:-0}"
CREDIT_SAMPLE_US="${CREDIT_SAMPLE_US:-1000}"
GOODPUT_SAMPLE_US="${GOODPUT_SAMPLE_US:-10000}"
STHR_SAMPLE_US="${STHR_SAMPLE_US:-10000}"

BDP_PKTS="${BDP_PKTS:-150}"
USE_SRR_SCHEDULING="${USE_SRR_SCHEDULING:-1}"
SIRD_SENDER_MD_FACTOR="${SIRD_SENDER_MD_FACTOR:-0.8}"
SIRD_SENDER_AI_STEP="${SIRD_SENDER_AI_STEP:-1.0}"
SIRD_SENDER_CREDIT_LAUNCH_DELAY_US="${SIRD_SENDER_CREDIT_LAUNCH_DELAY_US:-0}"
DEVICE_QUEUE_MAX_SIZE="${DEVICE_QUEUE_MAX_SIZE:-17p}"
QDISC_MAX_SIZE="${QDISC_MAX_SIZE:-1000p}"

LOW_STHR_FACTOR="${LOW_STHR_FACTOR:-0.1}"
MID_STHR_FACTOR="${MID_STHR_FACTOR:-0.5}"
HIGH_STHR_FACTOR="${HIGH_STHR_FACTOR:-4.0}"

LOW_STHR_PKTS="${LOW_STHR_PKTS:-$(awk "BEGIN { printf \"%d\", ($LOW_STHR_FACTOR * $BDP_PKTS) + 0.5 }")}"
MID_STHR_PKTS="${MID_STHR_PKTS:-$(awk "BEGIN { printf \"%d\", ($MID_STHR_FACTOR * $BDP_PKTS) + 0.5 }")}"
HIGH_STHR_PKTS="${HIGH_STHR_PKTS:-$(awk "BEGIN { printf \"%d\", ($HIGH_STHR_FACTOR * $BDP_PKTS) + 0.5 }")}"

if [[ -z "${TRACE_DIR+x}" ]]; then
  TS="$(date +%Y%m%d_%H%M%S)"
  DAY="$(date +%Y%m%d)"
  TRACE_DIR="$ROOT_DIR/outputs/sird-scenarios/${DAY}/HomaL4Protocol-bad3-sthr-sensitivity_${PROFILE}_${TS}"
fi

PLOT_DIR="${PLOT_DIR:-$TRACE_DIR/plots}"
mkdir -p "$TRACE_DIR" "$PLOT_DIR"

export LD_LIBRARY_PATH="$ROOT_DIR/build/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

BIN_PATH="$ROOT_DIR/build/scratch/bad3"
if [[ "$BUILD" == "1" || ! -x "$BIN_PATH" ]]; then
  echo "Building scratch/bad3..."
  ./waf build
fi

COMMON_ARGS=(
  "--outputDir=$TRACE_DIR"
  "--startSec=$START_SEC"
  "--durationSec=$DURATION_SEC"
  "--flowGapUs=$FLOW_GAP_US"
  "--activeReceiverCount=$ACTIVE_RECEIVER_COUNT"
  "--settleTailSec=$SETTLE_TAIL_SEC"
  "--backloggedFlow=1"
  "--backlogDepthMsgs=$BACKLOG_DEPTH_MSGS"
  "--msgSizeBytes=$MSG_SIZE_BYTES"
  "--traceMsg=$TRACE_MSG"
  "--traceProtocolCredit=$TRACE_PROTOCOL_CREDIT"
  "--traceCreditSample=$TRACE_CREDIT_SAMPLE"
  "--traceGoodput=$TRACE_GOODPUT"
  "--traceSthrSample=$TRACE_STHR_SAMPLE"
  "--traceSirdCredit=$TRACE_SIRD_CREDIT"
  "--traceSirdBucket=$TRACE_SIRD_BUCKET"
  "--traceSwitchEgressQueue=$TRACE_SWITCH_QUEUE"
  "--creditSampleUs=$CREDIT_SAMPLE_US"
  "--goodputSampleUs=$GOODPUT_SAMPLE_US"
  "--sthrSampleUs=$STHR_SAMPLE_US"
  "--bdpPkts=$BDP_PKTS"
  "--useSrrScheduling=$USE_SRR_SCHEDULING"
  "--sirdSenderMdFactor=$SIRD_SENDER_MD_FACTOR"
  "--sirdSenderAiStep=$SIRD_SENDER_AI_STEP"
  "--sirdSenderCreditLaunchDelayUs=$SIRD_SENDER_CREDIT_LAUNCH_DELAY_US"
  "--deviceQueueMaxSize=$DEVICE_QUEUE_MAX_SIZE"
  "--qdiscMaxSize=$QDISC_MAX_SIZE"
)

cat > "$TRACE_DIR/metadata.txt" <<EOF
profile=$PROFILE
startSec=$START_SEC
durationSec=$DURATION_SEC
flowGapUs=$FLOW_GAP_US
activeReceiverCount=$ACTIVE_RECEIVER_COUNT
settleTailSec=$SETTLE_TAIL_SEC
backlogDepthMsgs=$BACKLOG_DEPTH_MSGS
msgSizeBytes=$MSG_SIZE_BYTES
bdpPkts=$BDP_PKTS
useSrrScheduling=$USE_SRR_SCHEDULING
sirdSenderMdFactor=$SIRD_SENDER_MD_FACTOR
sirdSenderAiStep=$SIRD_SENDER_AI_STEP
sirdSenderCreditLaunchDelayUs=$SIRD_SENDER_CREDIT_LAUNCH_DELAY_US
creditSampleUs=$CREDIT_SAMPLE_US
goodputSampleUs=$GOODPUT_SAMPLE_US
sthrSampleUs=$STHR_SAMPLE_US
lowSthrFactor=$LOW_STHR_FACTOR
midSthrFactor=$MID_STHR_FACTOR
highSthrFactor=$HIGH_STHR_FACTOR
lowSthrPkts=$LOW_STHR_PKTS
midSthrPkts=$MID_STHR_PKTS
highSthrPkts=$HIGH_STHR_PKTS
EOF

run_case() {
  local tag="$1"
  local sthr_pkts="$2"
  local log_file="$TRACE_DIR/bad3_${tag}.run.log"

  rm -f \
    "$TRACE_DIR/bad3_${tag}.msg.tr" \
    "$TRACE_DIR/bad3_${tag}.credit-sample.tr" \
    "$TRACE_DIR/bad3_${tag}.goodput.tr" \
    "$TRACE_DIR/bad3_${tag}.sthr-sample.tr" \
    "$TRACE_DIR/bad3_${tag}.sird-credit.tr" \
    "$TRACE_DIR/bad3_${tag}.sender-credit.tr" \
    "$TRACE_DIR/bad3_${tag}.receiver-credit.tr"

  echo "[$tag] start $(date '+%F %T') sthrPkts=$sthr_pkts" | tee "$log_file"
  "$BIN_PATH" \
    "--simTag=$tag" \
    "--sirdSenderCsnThresholdPkts=$sthr_pkts" \
    "${COMMON_ARGS[@]}" >>"$log_file" 2>&1
  echo "[$tag] done $(date '+%F %T')" | tee -a "$log_file"
}

echo "profile=$PROFILE durationSec=$DURATION_SEC flowGapUs=$FLOW_GAP_US activeReceiverCount=$ACTIVE_RECEIVER_COUNT"
echo "SThr(low/mid/high)=$LOW_STHR_PKTS/$MID_STHR_PKTS/$HIGH_STHR_PKTS pkts"
echo "outputs: $TRACE_DIR"

declare -a CASES=(
  "low $LOW_STHR_PKTS"
  "mid $MID_STHR_PKTS"
  "high $HIGH_STHR_PKTS"
)

pids=()
for entry in "${CASES[@]}"; do
  read -r tag pkts <<<"$entry"
  run_case "$tag" "$pkts" &
  pids+=("$!")
  if [[ "${#pids[@]}" -ge "$SIM_JOBS" ]]; then
    wait "${pids[0]}"
    pids=("${pids[@]:1}")
  fi
done

status=0
if [[ "${#pids[@]}" -gt 0 ]]; then
  for pid in "${pids[@]}"; do
    if ! wait "$pid"; then
      status=1
    fi
  done
fi

if [[ "$status" -ne 0 ]]; then
  echo "At least one bad3 run failed. Check $TRACE_DIR/bad3_*.run.log"
  exit "$status"
fi

if [[ "$ANALYZE" == "1" ]]; then
  MPLBACKEND=Agg MPLCONFIGDIR="${MPLCONFIGDIR:-/tmp/mpl-bad3-cache}" \
  /usr/bin/python3 scripts/bad3_analyze.py \
    --root "$TRACE_DIR" \
    --out-dir "$PLOT_DIR"
fi

echo "bad3 runs finished."
