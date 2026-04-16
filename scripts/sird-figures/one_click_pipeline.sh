#!/usr/bin/env bash
set -euo pipefail

# One-click pipeline:
# 1) Run Homa and Homa+SIRD simulations
# 2) Parse traces and generate figures
# 3) Print generated artifact locations
#
# Usage:
#   bash scripts/sird-figures/one_click_pipeline.sh
#
# Optional overrides (environment variables):
#   LOADS="0.1,0.3,0.5"
#   SIM_IDX="0,1,2"
#   DURATION="0.02"
#   MODES="homa,sird"
#   TRACE_SIRD_GRANT="1"
#   TRACE_QUEUES="0"
#   DISABLE_RTX="0"
#   OUT_DIR="outputs/homa-paper-reproduction/figures-sird"

PROJECT_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"

LOADS="${LOADS:-0.1,0.3,0.5}"
SIM_IDX="${SIM_IDX:-0,1,2}"
DURATION="${DURATION:-0.02}"
MODES="${MODES:-homa,sird}"
TRACE_SIRD_GRANT="${TRACE_SIRD_GRANT:-1}"
TRACE_QUEUES="${TRACE_QUEUES:-0}"
DISABLE_RTX="${DISABLE_RTX:-0}"
OUT_DIR="${OUT_DIR:-outputs/homa-paper-reproduction/figures-sird}"

cd "$PROJECT_ROOT"

echo "[1/3] Running simulations"
RUN_ARGS=(
  --project-root .
  --loads "$LOADS"
  --sim-idx "$SIM_IDX"
  --duration "$DURATION"
  --modes "$MODES"
)

if [[ "$TRACE_SIRD_GRANT" == "1" ]]; then
  RUN_ARGS+=(--trace-sird-grant)
fi
if [[ "$TRACE_QUEUES" == "1" ]]; then
  RUN_ARGS+=(--trace-queues)
fi
if [[ "$DISABLE_RTX" == "1" ]]; then
  RUN_ARGS+=(--disable-rtx)
fi

python3 scripts/sird-figures/run_sird_experiments.py "${RUN_ARGS[@]}"

echo "[2/3] Plotting figures"
python3 scripts/sird-figures/plot_sird_figures.py \
  --trace-dir outputs/homa-paper-reproduction \
  --out-dir "$OUT_DIR"

echo "[3/3] Done"
echo "Figures:"
ls -1 "$OUT_DIR"/*.png
