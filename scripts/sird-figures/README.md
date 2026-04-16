# SIRD Figure Scripts

这组脚本用于在当前 ns-3 工程中批量跑 Homa/Homa+SIRD 对照，并画出论文风格图。

## 0) 一键执行（推荐）

在项目根目录执行：

```bash
bash scripts/sird-figures/one_click_pipeline.sh
```

可选环境变量（覆盖默认参数）：

```bash
LOADS=0.1,0.3,0.5 SIM_IDX=0,1,2 DURATION=0.02 MODES=homa,sird \
TRACE_SIRD_GRANT=1 TRACE_QUEUES=0 DISABLE_RTX=0 \
bash scripts/sird-figures/one_click_pipeline.sh
```

## 1) 批量跑仿真

在项目根目录执行：

```bash
python3 scripts/sird-figures/run_sird_experiments.py \
  --project-root . \
  --loads 0.1,0.3,0.5 \
  --sim-idx 0,1,2 \
  --duration 0.02 \
  --modes homa,sird \
  --trace-sird-grant
```

常用参数：
- `--modes`: `homa,sird` 或只跑其中一个
- `--trace-sird-grant`: 开启 SIRD grant 决策 trace 文件输出
- `--trace-queues`: 开启队列 trace（若需要后续队列图）
- `--sird-*`: SIRD 控制参数

## 2) 画图

```bash
python3 scripts/sird-figures/plot_sird_figures.py \
  --trace-dir outputs/homa-paper-reproduction \
  --out-dir outputs/homa-paper-reproduction/figures-sird
```

输出图（默认）：
- `fig_fct_cdf_homa_vs_sird.png`
- `fig_p99_fct_vs_load.png`
- `fig_goodput_vs_load.png`
- `fig_sird_grant_budget_timeline.png`（有 `.sird-grant.tr` 才生成）
- `fig_sird_csn_ratio_timeline.png`（有 `.sird-grant.tr` 才生成）

## 说明

- 这些图是“论文风格对照图”，重点用于快速验证迁移后的趋势。
- 若你想对齐原文全部图（含更多协议基线与拓扑配置），建议下一步把 DCTCP/Swift/dcPIM/ExpressPass 的同口径 trace 也统一接入本解析框架。
