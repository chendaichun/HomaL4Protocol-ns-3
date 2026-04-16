# SIRD 微基准场景说明（中文）

本目录提供两个与论文对应的微基准场景：

1. 接收端拥塞（Receiver Congestion / incast）
2. 发送端信息（Sender Information / outcast）

场景程序：
- `scratch/HomaL4Protocol-sird-microbench.cc`

运行脚本：
- `scripts/sird-scenarios/run_receiver_congestion.sh`
- `scripts/sird-scenarios/run_receiver_congestion_compare.sh`
- `scripts/sird-scenarios/run_sender_information.sh`

绘图脚本：
- `scripts/sird-scenarios/plot_sird_microbench.py`

## 1. 场景定义

### 1.1 接收端拥塞（incast）
- 6 个背景发送端持续向 1 个接收端发送 `10MB` 消息。
- 第 7 个发送端按固定周期发送 probe 消息（`8B` 或 `500KB`）。
- 关注点：在接收端成为瓶颈时，probe 消息延迟分布（CDF）。

### 1.2 发送端信息（outcast）
- 1 个发送端向 3 个接收端发送 `10MB` 消息。
- 三条流错峰启动（staggered）。
- 关注点：发送端 uplink 瓶颈下，`senderBudgetPkts` 是否随反馈收敛。

## 2. 为什么现在更快

脚本默认使用 `fast` 配置，并且直接调用已编译二进制，避免每次通过 `waf --run` 触发额外开销。

- `fast`（默认）：快速迭代、几分钟内完成
- `full`：更接近论文参数，运行更慢

## 3. 如何运行

在工程根目录 `HomaL4Protocol-ns-3` 下执行。

### 3.1 接收端拥塞场景
快速模式（默认）：
```bash
bash scripts/sird-scenarios/run_receiver_congestion.sh
```

完整版：
```bash
bash scripts/sird-scenarios/run_receiver_congestion.sh full
```

### 3.2 发送端信息场景
快速模式（默认）：
```bash
bash scripts/sird-scenarios/run_sender_information.sh
```

完整版：
```bash
bash scripts/sird-scenarios/run_sender_information.sh full
```

### 3.3 接收端拥塞 Homa vs SIRD 对比
快速模式（默认）：
```bash
bash scripts/sird-scenarios/run_receiver_congestion_compare.sh
```

完整版：
```bash
bash scripts/sird-scenarios/run_receiver_congestion_compare.sh full
```

## 4. 输出文件位置

原始 trace：
- `outputs/sird-scenarios/*.msg.tr`
- `outputs/sird-scenarios/*.sird-credit.tr`（兼容读取旧的 `*.sird-grant.tr`）

脚本会打印：
- 每个子实验的墙钟耗时
- 整个脚本总耗时

## 5. 作图

默认会把图片输出到 `output-f/`：
```bash
python3 scripts/sird-scenarios/plot_sird_microbench.py
```

也可以指定输入/输出目录：
```bash
python3 scripts/sird-scenarios/plot_sird_microbench.py \
  --trace-dir outputs/sird-scenarios \
  --out-dir output-f
```

## 6. 图的含义

- `receiver_congestion_probe_latency_cdf.png`
  - 比较 `8B` 与 `500KB` probe 消息在 incast 下的延迟 CDF。

- `receiver_congestion_homa_vs_sird_8B_cdf.png`
  - 在 `8B probe` 子场景下比较 Homa 与 SIRD 延迟 CDF。

- `receiver_congestion_homa_vs_sird_500KB_cdf.png`
  - 在 `500KB probe` 子场景下比较 Homa 与 SIRD 延迟 CDF。

- `receiver_congestion_sird_credit_share_500KB_timeseries.png`
  - 在 `500KB probe` 的 SIRD 运行中，按时间窗显示 `500KB` 与 `10MB` 消息拿到的 credit 份额。

- `sender_information_budget_timeseries.png`
  - 比较 outcast 场景下两种配置的 `senderBudgetPkts` 随时间变化。

## 7. 常见问题

1. 没有二进制
- 运行脚本时会自动检测；若不存在会先执行一次 `./waf build`。

2. 图生成失败
- 确认已安装 `matplotlib`：
  - `python3 -c "import matplotlib"`

3. 想进一步缩短时间
- 继续使用 `fast` 配置。
- 也可以在脚本里调小 `durationSec` 或增大 probe 间隔。