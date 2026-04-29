#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

PROFILE="${1:-fast}"

START_SEC="${START_SEC:-0.2}"
DURATION_SEC="${DURATION_SEC:-2.0}"
SETTLE_TAIL_SEC="${SETTLE_TAIL_SEC:-0.1}"
MSG_SIZE_BYTES="${MSG_SIZE_BYTES:-10000000}"
BACKLOG_DEPTH_MSGS="${BACKLOG_DEPTH_MSGS:-8}"

if [[ "$PROFILE" == "smoke" ]]; then
  DURATION_SEC="${DURATION_SEC_SMOKE:-0.2}"
  SETTLE_TAIL_SEC="${SETTLE_TAIL_SEC_SMOKE:-0.05}"
elif [[ "$PROFILE" == "full" ]]; then
  DURATION_SEC="${DURATION_SEC_FULL:-8.0}"
  SETTLE_TAIL_SEC="${SETTLE_TAIL_SEC_FULL:-0.2}"
elif [[ "$PROFILE" != "fast" ]]; then
  echo "Usage: bash scripts/bad2.sh [smoke|fast|full]"
  exit 2
fi

BUILD="${BUILD:-1}"
ANALYZE="${ANALYZE:-1}"
HOST_RATE_GBPS="${HOST_RATE_GBPS:-100}"
INTER_SWITCH_RATE_GBPS="${INTER_SWITCH_RATE_GBPS:-100}"
BOTTLENECK_RATE_GBPS="${BOTTLENECK_RATE_GBPS:-100}"
SHORT_PATH_DELAY_US="${SHORT_PATH_DELAY_US:-1}"
LONG_PATH_EXTRA_DELAYS_US="${LONG_PATH_EXTRA_DELAYS_US:-0 4 8 16 32}"
read -r -a LONG_PATH_EXTRA_DELAYS_ARR <<< "$LONG_PATH_EXTRA_DELAYS_US"

TRACE_MSG="${TRACE_MSG:-0}"
TRACE_PROTOCOL_CREDIT="${TRACE_PROTOCOL_CREDIT:-0}"
TRACE_CREDIT_SAMPLE="${TRACE_CREDIT_SAMPLE:-1}"
TRACE_GOODPUT="${TRACE_GOODPUT:-1}"
TRACE_SWITCH_QUEUE="${TRACE_SWITCH_QUEUE:-1}"
CREDIT_SAMPLE_US="${CREDIT_SAMPLE_US:-500}"
GOODPUT_SAMPLE_US="${GOODPUT_SAMPLE_US:-1000}"
TRACE_SWITCH_QUEUE_SAMPLE_US="${TRACE_SWITCH_QUEUE_SAMPLE_US:-500}"

BDP_PKTS="${BDP_PKTS:-33.32}"
SIRD_SENDER_CREDIT_LAUNCH_DELAY_US="${SIRD_SENDER_CREDIT_LAUNCH_DELAY_US:-0}"
USE_SRR_SCHEDULING="${USE_SRR_SCHEDULING:-0}"

DEVICE_QUEUE_MAX_SIZE="${DEVICE_QUEUE_MAX_SIZE:-2000p}"
QDISC_MAX_SIZE="${QDISC_MAX_SIZE:-1000p}"

if [[ -z "${TRACE_DIR+x}" ]]; then
  TS="$(date +%Y%m%d_%H%M%S)"
  DAY="$(date +%Y%m%d)"
  TRACE_DIR="$ROOT_DIR/outputs/sird-scenarios/${DAY}/HomaL4Protocol-bad2-rtt-heterogeneity_${PROFILE}_${TS}"
fi
mkdir -p "$TRACE_DIR"

export LD_LIBRARY_PATH="$ROOT_DIR/build/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

BIN_PATH="$ROOT_DIR/build/scratch/bad2"
if [[ "$BUILD" == "1" || ! -x "$BIN_PATH" ]]; then
  echo "Building scratch/bad2..."
  ./waf build
fi

COMMON_ARGS=(
  "--outputDir=$TRACE_DIR"
  "--startSec=$START_SEC"
  "--durationSec=$DURATION_SEC"
  "--settleTailSec=$SETTLE_TAIL_SEC"
  "--msgSizeBytes=$MSG_SIZE_BYTES"
  "--backlogDepthMsgs=$BACKLOG_DEPTH_MSGS"
  "--hostRateGbps=$HOST_RATE_GBPS"
  "--interSwitchRateGbps=$INTER_SWITCH_RATE_GBPS"
  "--bottleneckRateGbps=$BOTTLENECK_RATE_GBPS"
  "--shortPathDelayUs=$SHORT_PATH_DELAY_US"
  "--traceMsg=$TRACE_MSG"
  "--traceProtocolCredit=$TRACE_PROTOCOL_CREDIT"
  "--traceCreditSample=$TRACE_CREDIT_SAMPLE"
  "--traceGoodput=$TRACE_GOODPUT"
  "--traceSwitchEgressQueue=$TRACE_SWITCH_QUEUE"
  "--creditSampleUs=$CREDIT_SAMPLE_US"
  "--goodputSampleUs=$GOODPUT_SAMPLE_US"
  "--switchQueueSampleUs=$TRACE_SWITCH_QUEUE_SAMPLE_US"
  "--bdpPkts=$BDP_PKTS"
  "--sirdSenderCreditLaunchDelayUs=$SIRD_SENDER_CREDIT_LAUNCH_DELAY_US"
  "--useSrrScheduling=$USE_SRR_SCHEDULING"
  "--deviceQueueMaxSize=$DEVICE_QUEUE_MAX_SIZE"
  "--qdiscMaxSize=$QDISC_MAX_SIZE"
)

cat > "$TRACE_DIR/metadata.txt" <<EOF
profile=$PROFILE
startSec=$START_SEC
durationSec=$DURATION_SEC
settleTailSec=$SETTLE_TAIL_SEC
msgSizeBytes=$MSG_SIZE_BYTES
backlogDepthMsgs=$BACKLOG_DEPTH_MSGS
hostRateGbps=$HOST_RATE_GBPS
interSwitchRateGbps=$INTER_SWITCH_RATE_GBPS
bottleneckRateGbps=$BOTTLENECK_RATE_GBPS
shortPathDelayUs=$SHORT_PATH_DELAY_US
longPathExtraDelaysUs=$LONG_PATH_EXTRA_DELAYS_US
bdpPkts=$BDP_PKTS
traceMsg=$TRACE_MSG
traceProtocolCredit=$TRACE_PROTOCOL_CREDIT
traceCreditSample=$TRACE_CREDIT_SAMPLE
traceGoodput=$TRACE_GOODPUT
traceSwitchQueue=$TRACE_SWITCH_QUEUE
creditSampleUs=$CREDIT_SAMPLE_US
goodputSampleUs=$GOODPUT_SAMPLE_US
switchQueueSampleUs=$TRACE_SWITCH_QUEUE_SAMPLE_US
EOF

run_case() {
  local delay_label="$1"
  local long_extra_delay_us="$2"
  local tag="extra${delay_label}us"
  local log_file="$TRACE_DIR/bad2_${tag}.run.log"

  rm -f \
    "$TRACE_DIR/bad2_${tag}.summary.tr" \
    "$TRACE_DIR/bad2_${tag}.msg.tr" \
    "$TRACE_DIR/bad2_${tag}.sender-credit.tr" \
    "$TRACE_DIR/bad2_${tag}.receiver-credit.tr" \
    "$TRACE_DIR/bad2_${tag}.credit-sample.tr" \
    "$TRACE_DIR/bad2_${tag}.goodput.tr" \
    "$TRACE_DIR/bad2_${tag}.switch-egress-queue.tr"

  echo "[$tag] start $(date '+%F %T') longPathExtraDelayUs=$long_extra_delay_us" | tee "$log_file"
  "$BIN_PATH" \
    "--simTag=$tag" \
    "--longPathExtraDelayUs=$long_extra_delay_us" \
    "${COMMON_ARGS[@]}" >>"$log_file" 2>&1
  echo "[$tag] done $(date '+%F %T')" | tee -a "$log_file"
}

echo "profile=$PROFILE durationSec=$DURATION_SEC bottleneckRateGbps=$BOTTLENECK_RATE_GBPS"
echo "longPathExtraDelaysUs=$LONG_PATH_EXTRA_DELAYS_US"
echo "outputs: $TRACE_DIR"

for long_extra_delay_us in "${LONG_PATH_EXTRA_DELAYS_ARR[@]}"; do
  delay_label="${long_extra_delay_us//./p}"
  run_case "$delay_label" "$long_extra_delay_us"
done

if [[ "$ANALYZE" == "1" ]]; then
  /usr/bin/python3 scripts/bad2_analyze.py \
    --root "$TRACE_DIR" \
    --out-csv "$TRACE_DIR/bad2_summary.csv" \
    --out-md "$TRACE_DIR/bad2_analysis.md" \
    --plot-dir "$TRACE_DIR/plots"
fi

echo "bad2 runs finished."
