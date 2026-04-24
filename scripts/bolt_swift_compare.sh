#!/usr/bin/env zsh
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

mkdir -p "$ROOT_DIR/outputs"
OUT_DIR="$(mktemp -d "outputs/bolt-swift-compare-XXXXXXXX")"

run_mode() {
  local mode="$1"
  local log_file="$2"
  local queue_file="$3"

  ./waf --run "bolt-swift-compare --ccMode=$mode --queueTrace=$queue_file" \
    >"$log_file" 2>&1
}

run_mode "DEFAULT" "$OUT_DIR/default.log" "$OUT_DIR/default-queue.csv"
run_mode "SWIFT" "$OUT_DIR/swift.log" "$OUT_DIR/swift-queue.csv"

echo "$OUT_DIR"
