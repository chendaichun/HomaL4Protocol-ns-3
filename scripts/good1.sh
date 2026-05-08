#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

PROFILE="${1:-fast}"

START_SEC="${START_SEC:-0.2}"
DURATION_SEC="${DURATION_SEC:-0.006}"
SETTLE_TAIL_SEC="${SETTLE_TAIL_SEC:-0.02}"
SHORT_INTERVAL_8B_US="${SHORT_INTERVAL_8B_US:-100}"
SHORT_INTERVAL_500KB_US="${SHORT_INTERVAL_500KB_US:-1000}"
TARGET_PROBE_MESSAGES="${TARGET_PROBE_MESSAGES:-2000}"

if [[ "$PROFILE" == "smoke" ]]; then
  DURATION_SEC="${DURATION_SEC_SMOKE:-0.003}"
  SETTLE_TAIL_SEC="${SETTLE_TAIL_SEC_SMOKE:-0.01}"
  TARGET_PROBE_MESSAGES="${TARGET_PROBE_MESSAGES_SMOKE:-400}"
elif [[ "$PROFILE" == "full" ]]; then
  DURATION_SEC="${DURATION_SEC_FULL:-0.02}"
  SETTLE_TAIL_SEC="${SETTLE_TAIL_SEC_FULL:-0.05}"
  TARGET_PROBE_MESSAGES="${TARGET_PROBE_MESSAGES_FULL:-4000}"
elif [[ "$PROFILE" != "fast" ]]; then
  echo "Usage: bash scripts/good1.sh [smoke|fast|full]"
  exit 2
fi

LONG_MSG_SIZE_BYTES="${LONG_MSG_SIZE_BYTES:-10000000}"
LONG_SENDER_RATE_GBPS="${LONG_SENDER_RATE_GBPS:-16.67}"
BUILD="${BUILD:-1}"
PLOT="${PLOT:-1}"
TRACE_QUEUE="${TRACE_QUEUE:-1}"
TRACE_QUEUE_SAMPLE_US="${TRACE_QUEUE_SAMPLE_US:-1}"
TRACE_SIRD_CREDIT="${TRACE_SIRD_CREDIT:-0}"
TRACE_SIRD_LOOP="${TRACE_SIRD_LOOP:-0}"
BDP_PKTS="${BDP_PKTS:-33.32}"
DEVICE_QUEUE_MAX_SIZE="${DEVICE_QUEUE_MAX_SIZE:-1000p}"
QDISC_MAX_SIZE="${QDISC_MAX_SIZE:-1000p}"

if [[ -z "${OUT_DIR+x}" ]]; then
  TS="$(date +%Y%m%d_%H%M%S)"
  DAY="$(date +%Y%m%d)"
  OUT_DIR="outputs/sird-scenarios/${DAY}/HomaL4Protocol-good1-receiver-bottleneck_${PROFILE}_${TS}"
fi
if [[ "$OUT_DIR" == *" "* ]]; then
  echo "OUT_DIR must not contain spaces: $OUT_DIR"
  exit 2
fi
PLOT_OUT_DIR="${PLOT_OUT_DIR:-$OUT_DIR/plots}"
REPORT_PATH="${REPORT_PATH:-$OUT_DIR/good1_report_zh.md}"
mkdir -p "$OUT_DIR"

export LD_LIBRARY_PATH="$ROOT_DIR/build/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

BIN_PATH="$ROOT_DIR/build/scratch/good1"
if [[ "$BUILD" == "1" || ! -x "$BIN_PATH" ]]; then
  echo "Building scratch/good1..."
  ./waf build
fi

COMMON_ARGS=(
  "--enableSird=1"
  "--outputDir=$OUT_DIR"
  "--startSec=$START_SEC"
  "--longMsgSizeBytes=$LONG_MSG_SIZE_BYTES"
  "--longSenderRateGbps=$LONG_SENDER_RATE_GBPS"
  "--bdpPkts=$BDP_PKTS"
  "--traceMsg=1"
  "--tracePathRtt=0"
  "--traceSwitchEgressQueue=$TRACE_QUEUE"
  "--switchQueueSampleUs=$TRACE_QUEUE_SAMPLE_US"
  "--traceLinkThroughput=1"
  "--traceSirdCredit=$TRACE_SIRD_CREDIT"
  "--traceSirdLoop=$TRACE_SIRD_LOOP"
  "--deviceQueueMaxSize=$DEVICE_QUEUE_MAX_SIZE"
  "--qdiscMaxSize=$QDISC_MAX_SIZE"
  "--showProgressBar=0"
)

run_case() {
  local label="$1"
  local probe_size="$2"
  local enable_background="$3"
  local use_srr="$4"
  local short_interval_us="$5"
  local tag="good1_${PROFILE}_${label}"
  local log_file="$OUT_DIR/${tag}.run.log"
  local duration_sec="$DURATION_SEC"

  if [[ "$TARGET_PROBE_MESSAGES" -gt 0 ]]; then
    duration_sec=$(awk -v n="$TARGET_PROBE_MESSAGES" -v us="$short_interval_us" 'BEGIN {printf "%.9f", n * us / 1000000.0}')
  fi

  rm -f \
    "$OUT_DIR/lab1_${tag}.msg.tr" \
    "$OUT_DIR/lab1_${tag}.link-throughput.tr" \
    "$OUT_DIR/lab1_${tag}.switch-egress-queue.tr"

  echo "[$tag] start $(date '+%F %T')" | tee "$log_file"
  "$BIN_PATH" \
    "--simTag=$tag" \
    "--shortMsgSizeBytes=$probe_size" \
    "--shortIntervalUs=$short_interval_us" \
    "--durationSec=$duration_sec" \
    "--targetProbeMessages=$TARGET_PROBE_MESSAGES" \
    "--enableBackgroundTraffic=$enable_background" \
    "--useSrrScheduling=$use_srr" \
    "--settleTailSec=$SETTLE_TAIL_SEC" \
    "${COMMON_ARGS[@]}" >>"$log_file" 2>&1
  echo "[$tag] done $(date '+%F %T')" | tee -a "$log_file"
}

run_case "unloaded_8B" 8 0 0 "$SHORT_INTERVAL_8B_US"
run_case "incast_8B_srpt" 8 1 0 "$SHORT_INTERVAL_8B_US"
run_case "unloaded_500KB" 500000 0 0 "$SHORT_INTERVAL_500KB_US"
run_case "incast_500KB_srpt" 500000 1 0 "$SHORT_INTERVAL_500KB_US"

if [[ "$PLOT" == "1" ]]; then
  mkdir -p "$PLOT_OUT_DIR"
  python3 scripts/lab1_plot.py \
    --trace-dir "$OUT_DIR" \
    --out-dir "$PLOT_OUT_DIR" \
    --tag "good1_${PROFILE}_unloaded_8B" \
    --tag "good1_${PROFILE}_incast_8B_srpt" \
    --tag "good1_${PROFILE}_unloaded_500KB" \
    --tag "good1_${PROFILE}_incast_500KB_srpt" \
    --size 8 \
    --size 500000 \
    --start-sec "$START_SEC"
fi

cat > "$REPORT_PATH" <<'EOF'
# good1：接收端下行瓶颈场景

## 场景

`good1` 用于展示典型 incast 下的 receiver bottleneck。6 个背景发送端持续向同一接收端发送 `10MB` 长消息，同时引入 1 个 probe 发送端，分别测试 `8B` 与 `500KB` 两类消息。

本场景的对照采用方案 B：

- `unloaded`：无背景长流，作为基线；
- `incast + SRPT`：接收端成为瓶颈，由 SIRD/Homa 的 credit 机制驱动接收端注入控制。

## 主要图

### 1. 8B 短消息时延 CDF

![8B latency CDF](plots/lab1_fct_cdf_8B.png)

这张图用于看 receiver bottleneck 出现后，极短消息的完成时间是否仍然接近 unloaded 基线。如果两条曲线接近，说明接收端 credit 控制没有把短消息淹没在大消息排队中。

### 2. 500KB 消息时延 CDF

![500KB latency CDF](plots/lab1_fct_cdf_500KB.png)

这张图用于看中等消息在 incast 下是否仍能获得较好的完成时间。若 `incast + SRPT` 明显优于“简单排队直发”的直觉结果，说明接收端调度确实在起作用。

### 3. 接收端队列时间序列

![Receiver queue timeseries](plots/lab1_receiver_queue_pkts_timeseries.png)

这张图直接回答“下行瓶颈是不是因为队列失控”。如果队列没有长期失控增长，而 probe 时延仍保持可接受，说明 credit 机制限制了 receiver-facing 注入。

### 4. 接收端吞吐时间序列

![Receiver throughput timeseries](plots/lab1_receiver_throughput_timeseries.png)

这张图用于说明 receiver-facing 链路在 incast 下被持续利用，同时没有依赖过度排队来维持吞吐。

## 可以据此写出的结论

1. 在 receiver bottleneck / incast 场景中，SIRD 通过 credit 控制接收端注入速率，避免 receiver-facing 队列长期堆积。
2. 极短消息（8B）在强背景流量下仍能保持接近 unloaded 的时延，说明短消息 fast path 没有被大消息淹没。
3. 中等消息（500KB）在接收端受限时仍可被及时推进，说明 receiver-driven scheduling 不只是“限速”，还在做消息优先级上的有效选择。

## 输出目录

- trace: 当前实验输出目录
- plots: 当前实验输出目录下的 `plots/`
EOF

echo "good1 finished. report: $REPORT_PATH"
