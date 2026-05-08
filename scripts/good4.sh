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
  echo "Usage: bash scripts/good4.sh [smoke|fast|full]"
  exit 2
fi

LONG_MSG_SIZE_BYTES="${LONG_MSG_SIZE_BYTES:-10000000}"
LONG_SENDER_RATE_GBPS="${LONG_SENDER_RATE_GBPS:-16.67}"
BUILD="${BUILD:-1}"
PLOT="${PLOT:-1}"
TRACE_QUEUE="${TRACE_QUEUE:-0}"
TRACE_QUEUE_SAMPLE_US="${TRACE_QUEUE_SAMPLE_US:-1}"
BDP_PKTS="${BDP_PKTS:-33.32}"
DEVICE_QUEUE_MAX_SIZE="${DEVICE_QUEUE_MAX_SIZE:-1000p}"
QDISC_MAX_SIZE="${QDISC_MAX_SIZE:-1000p}"

if [[ -z "${OUT_DIR+x}" ]]; then
  TS="$(date +%Y%m%d_%H%M%S)"
  DAY="$(date +%Y%m%d)"
  OUT_DIR="outputs/sird-scenarios/${DAY}/HomaL4Protocol-good4-mixed-sizes_${PROFILE}_${TS}"
fi
if [[ "$OUT_DIR" == *" "* ]]; then
  echo "OUT_DIR must not contain spaces: $OUT_DIR"
  exit 2
fi
PLOT_OUT_DIR="${PLOT_OUT_DIR:-$OUT_DIR/plots}"
REPORT_PATH="${REPORT_PATH:-$OUT_DIR/good4_report_zh.md}"
mkdir -p "$OUT_DIR"

export LD_LIBRARY_PATH="$ROOT_DIR/build/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

BIN_PATH="$ROOT_DIR/build/scratch/good4"
if [[ "$BUILD" == "1" || ! -x "$BIN_PATH" ]]; then
  echo "Building scratch/good4..."
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
  "--traceSirdCredit=0"
  "--traceSirdLoop=0"
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
  local tag="good4_${PROFILE}_${label}"
  local log_file="$OUT_DIR/${tag}.run.log"
  local duration_sec="$DURATION_SEC"

  if [[ "$TARGET_PROBE_MESSAGES" -gt 0 ]]; then
    duration_sec=$(awk -v n="$TARGET_PROBE_MESSAGES" -v us="$short_interval_us" 'BEGIN {printf "%.9f", n * us / 1000000.0}')
  fi

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
run_case "incast_500KB_srr" 500000 1 1 "$SHORT_INTERVAL_500KB_US"

if [[ "$PLOT" == "1" ]]; then
  mkdir -p "$PLOT_OUT_DIR"
  python3 scripts/lab1_plot.py \
    --trace-dir "$OUT_DIR" \
    --out-dir "$PLOT_OUT_DIR" \
    --tag "good4_${PROFILE}_unloaded_8B" \
    --tag "good4_${PROFILE}_incast_8B_srpt" \
    --tag "good4_${PROFILE}_unloaded_500KB" \
    --tag "good4_${PROFILE}_incast_500KB_srpt" \
    --tag "good4_${PROFILE}_incast_500KB_srr" \
    --size 8 \
    --size 500000 \
    --start-sec "$START_SEC"
fi

cat > "$REPORT_PATH" <<'EOF'
# good4：混合消息大小场景

## 场景

`good4` 用于强调 mixed message size workload 下的两种现象：

1. 极短消息（8B）可以沿 unscheduled fast path 快速完成；
2. 中等消息（500KB）虽然需要更多 scheduled data，但在 SRPT 下可显著优于 SRR。

场景仍使用 receiver-side incast 拓扑，但本报告不再强调“receiver 是否成为瓶颈”，而是强调“不同大小消息在同一机制下被如何区别对待”。

## 主要图

### 1. 8B 时延 CDF

![8B latency CDF](plots/lab1_fct_cdf_8B.png)

这张图用于说明极短消息是否接近 unloaded 基线。若差距很小，说明短消息不需要等待大量 credit 周转即可快速完成。

### 2. 500KB 时延 CDF

![500KB latency CDF](plots/lab1_fct_cdf_500KB.png)

这张图同时包含 `unloaded`、`incast + SRPT`、`incast + SRR`。它直接体现 receiver scheduling policy 对中等消息完成时间的影响。

## 可以据此写出的结论

1. 对于 8B 这类极短消息，SIRD/Homa 风格的 unscheduled fast path 使其在混合负载中仍能保持接近基线的完成时间。
2. 对于 500KB 这类非极短消息，receiver-side scheduling policy 会显著影响完成时间。
3. SRPT 优先推进剩余字节更少的消息，因此更有利于 500KB 消息；SRR 更强调 sender 间公平，因此会牺牲这类消息的完成时间。

## 输出目录

- trace: 当前实验输出目录
- plots: 当前实验输出目录下的 `plots/`
EOF

echo "good4 finished. report: $REPORT_PATH"
