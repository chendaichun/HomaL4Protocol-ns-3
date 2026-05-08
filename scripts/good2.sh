#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

PROFILE="${1:-fast}"

START_SEC="${START_SEC:-0.2}"
DURATION_SEC="${DURATION_SEC:-0.6}"
FLOW_GAP_US="${FLOW_GAP_US:-200000}"
ACTIVE_RECEIVER_COUNT="${ACTIVE_RECEIVER_COUNT:-3}"
SETTLE_TAIL_SEC="${SETTLE_TAIL_SEC:-0.1}"
SEND_INTERVAL_US="${SEND_INTERVAL_US:-400}"
BACKLOGGED_FLOW="${BACKLOGGED_FLOW:-1}"
BACKLOG_DEPTH_MSGS="${BACKLOG_DEPTH_MSGS:-2}"

if [[ "$PROFILE" == "smoke" ]]; then
  DURATION_SEC="${DURATION_SEC_SMOKE:-0.15}"
  FLOW_GAP_US="${FLOW_GAP_US_SMOKE:-50000}"
  SETTLE_TAIL_SEC="${SETTLE_TAIL_SEC_SMOKE:-0.05}"
elif [[ "$PROFILE" == "full" ]]; then
  DURATION_SEC="${DURATION_SEC_FULL:-13.2}"
  FLOW_GAP_US="${FLOW_GAP_US_FULL:-4500000}"
  SETTLE_TAIL_SEC="${SETTLE_TAIL_SEC_FULL:-0.1}"
elif [[ "$PROFILE" != "fast" ]]; then
  echo "Usage: bash scripts/good2.sh [smoke|fast|full]"
  exit 2
fi

BUILD="${BUILD:-1}"
PLOT="${PLOT:-1}"
OUTCAST_MSG_SIZE_BYTES="${OUTCAST_MSG_SIZE_BYTES:-10000000}"
TRACE_MSG="${TRACE_MSG:-0}"
TRACE_PROTOCOL_CREDIT="${TRACE_PROTOCOL_CREDIT:-0}"
TRACE_CREDIT_SAMPLE="${TRACE_CREDIT_SAMPLE:-1}"
TRACE_SIRD_CREDIT="${TRACE_SIRD_CREDIT:-0}"
TRACE_SIRD_BUCKET="${TRACE_SIRD_BUCKET:-0}"
TRACE_CREDIT_EVENTS="${TRACE_CREDIT_EVENTS:-1}"
TRACE_SWITCH_QUEUE="${TRACE_SWITCH_QUEUE:-1}"
CREDIT_SAMPLE_US="${CREDIT_SAMPLE_US:-100}"
TRACE_SWITCH_QUEUE_SAMPLE_US="${TRACE_SWITCH_QUEUE_SAMPLE_US:-1000}"
BDP_PKTS="${BDP_PKTS:-150}"
SIRD_CSN_THRESHOLD_PKTS="${SIRD_CSN_THRESHOLD_PKTS:-$(awk "BEGIN { printf \"%d\", (0.5 * $BDP_PKTS) + 0.5 }")}"
SIRD_INF_CSN_THRESHOLD_PKTS="${SIRD_INF_CSN_THRESHOLD_PKTS:-65535}"
DEVICE_QUEUE_MAX_SIZE="${DEVICE_QUEUE_MAX_SIZE:-17p}"
QDISC_MAX_SIZE="${QDISC_MAX_SIZE:-1000p}"
SIRD_SENDER_CREDIT_LAUNCH_DELAY_US="${SIRD_SENDER_CREDIT_LAUNCH_DELAY_US:-0}"
USE_SRR_SCHEDULING="${USE_SRR_SCHEDULING:-1}"

if [[ -z "${TRACE_DIR+x}" ]]; then
  TS="$(date +%Y%m%d_%H%M%S)"
  DAY="$(date +%Y%m%d)"
  TRACE_DIR="$ROOT_DIR/outputs/sird-scenarios/${DAY}/HomaL4Protocol-good2-sender-bottleneck_${PROFILE}_${TS}"
fi
PLOT_OUT_DIR="${PLOT_OUT_DIR:-$TRACE_DIR/plots}"
REPORT_PATH="${REPORT_PATH:-$TRACE_DIR/good2_report_zh.md}"
mkdir -p "$TRACE_DIR"

export LD_LIBRARY_PATH="$ROOT_DIR/build/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

BIN_PATH="$ROOT_DIR/build/scratch/good2"
if [[ "$BUILD" == "1" || ! -x "$BIN_PATH" ]]; then
  echo "Building scratch/good2..."
  ./waf build
fi

COMMON_ARGS=(
  "--enableSird=1"
  "--outputDir=$TRACE_DIR"
  "--traceMsg=$TRACE_MSG"
  "--traceProtocolCredit=$TRACE_PROTOCOL_CREDIT"
  "--traceCreditSample=$TRACE_CREDIT_SAMPLE"
  "--traceSirdCredit=$TRACE_SIRD_CREDIT"
  "--traceSirdBucket=$TRACE_SIRD_BUCKET"
  "--traceCreditEvents=$TRACE_CREDIT_EVENTS"
  "--traceSwitchEgressQueue=$TRACE_SWITCH_QUEUE"
  "--creditSampleUs=$CREDIT_SAMPLE_US"
  "--switchQueueSampleUs=$TRACE_SWITCH_QUEUE_SAMPLE_US"
  "--bdpPkts=$BDP_PKTS"
  "--startSec=$START_SEC"
  "--durationSec=$DURATION_SEC"
  "--settleTailSec=$SETTLE_TAIL_SEC"
  "--msgSizeBytes=$OUTCAST_MSG_SIZE_BYTES"
  "--flowGapUs=$FLOW_GAP_US"
  "--sendIntervalUs=$SEND_INTERVAL_US"
  "--backloggedFlow=$BACKLOGGED_FLOW"
  "--backlogDepthMsgs=$BACKLOG_DEPTH_MSGS"
  "--activeReceiverCount=$ACTIVE_RECEIVER_COUNT"
  "--deviceQueueMaxSize=$DEVICE_QUEUE_MAX_SIZE"
  "--qdiscMaxSize=$QDISC_MAX_SIZE"
  "--sirdSenderCreditLaunchDelayUs=$SIRD_SENDER_CREDIT_LAUNCH_DELAY_US"
  "--useSrrScheduling=$USE_SRR_SCHEDULING"
)

run_case() {
  local tag="$1"
  local csn_threshold="$2"
  local sender_md="$3"
  local sender_ai="$4"
  local log_file="$TRACE_DIR/good2_${tag}.run.log"

  rm -f \
    "$TRACE_DIR/lab2_${tag}.credit-sample.tr" \
    "$TRACE_DIR/lab2_${tag}.credit-events.tr" \
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

run_case "feedback" "$SIRD_CSN_THRESHOLD_PKTS" 0.8 1.0
run_case "no_feedback" "$SIRD_INF_CSN_THRESHOLD_PKTS" 1.0 0.0

if [[ "$PLOT" == "1" ]]; then
  mkdir -p "$PLOT_OUT_DIR"
  python3 scripts/lab2_plot.py \
    --trace-dir "$TRACE_DIR" \
    --out-dir "$PLOT_OUT_DIR" \
    --feedback-tag feedback \
    --no-feedback-tag no_feedback \
    --bdp-pkts "$BDP_PKTS" \
    --sample-us "$CREDIT_SAMPLE_US" \
    --start-sec "$START_SEC"
fi

cat > "$REPORT_PATH" <<'EOF'
# good2：发送端上行瓶颈场景

## 场景

`good2` 构造单发送端、三个接收端按时间错峰加入的 sender bottleneck 场景。发送端持续发送 `10MB` 大消息，重点观察 receiver 是否能够通过 sender-side feedback 感知“sender 本身已经成为瓶颈”。

方案 B 的对照为：

- `feedback`：正常启用 sender feedback；
- `no_feedback`：把 `S_Thr` 设到极大，同时关闭 sender-side AIMD，使 receiver 近似看不到 sender 积压。

## 主要图

### 1. Sender / Receiver credit 动态

![Sender credit dynamics](plots/lab2_sender_credit_dynamics.png)

这张图更适合解释随时间变化的过程，尤其是第二个、第三个 receiver 加入之后，sender-held credit 是否出现阶梯式堆积。

## 可以据此写出的结论

1. 当 sender uplink 成为瓶颈时，sender feedback 能帮助 receiver 识别“继续发 credit 也不会更快发出数据”的 sender。
2. 开启 sender feedback 后，sender-held credit 被限制在较低水平，而 receiver 侧仍保留更多可再分配的 credit。
3. 关闭 sender feedback 后，多个 receiver 会继续把 credit 发给同一个 sender，导致 credit 在 sender 端堆积，形成低效占用。

## 输出目录

- trace: 当前实验输出目录
- plots: 当前实验输出目录下的 `plots/`
EOF

echo "good2 finished. report: $REPORT_PATH"
