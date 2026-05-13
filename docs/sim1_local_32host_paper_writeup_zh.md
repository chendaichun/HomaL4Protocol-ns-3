# 本地 sim1 纸面窗口实验报告：32 主机 leaf-spine 场景

## 1. 这次实验是什么

这是一次**本地小规模 sim1 验证**，目标不是严格复现原文 144 hosts 的大规模数值，而是验证：

1. 纸面窗口是否按原文思路切分；
2. `goodput`、`ToR queue`、`msg.tr` 是否都能稳定导出；
3. `balanced / core / incast` 三类场景是否能跑出符合直觉的趋势。

本次结果目录：

```text
outputs/sim1/20260508/sim1_32h_4tor_2spine_paperwindow_fullparallel_20260508_211824
```

完成时间大约是：

```text
2026-05-09 01:39:51
```

## 2. 场景设置

拓扑如下：

- 32 hosts
- 4 ToR
- 2 spine
- 每个 ToR 下 8 台主机
- host-ToR 链路：100 Gbps
- `deviceQueueMaxSize=1p`
- `torQueueIncludeDevice=0`

三类 traffic config：

- `balanced`
- `core`
- `incast`

三类 workload：

- `google_rpc`
- `facebook_hadoop`
- `web_search`

纸面窗口按原文风格切：

| workload | traffic_duration_sec | trace_start_sec | trace_duration_sec |
|---|---:|---:|---:|
| google_rpc | 0.18 | 10.162 | 0.018 |
| facebook_hadoop | 0.8 | 10.72 | 0.08 |
| web_search | 1.0 | 10.9 | 0.1 |

`goodput` 采样间隔为 `1ms`。

## 3. 指标语义

本次报告里的主指标是：

- `paper_max_goodput_gbps`：统计窗口内的 per-host request goodput 峰值
- `paper_mean_sample_goodput_gbps`：统计窗口内的平均采样 goodput
- `paper_run_avg_goodput_gbps`：按完成字节数反推的窗口平均 per-host goodput
- `paper_max_tor_queue_mb`：ToR egress queue 峰值

说明：

- 这里的 `goodput` 采用 request-only 口径；
- `ToR queue` 采用 peak summary 口径，不是时间序列均值；
- `msg.tr` 仍然保留，用于后续算 FCT / slowdown。

## 4. 结果汇总

### 4.1 google_rpc

| traffic config | max goodput (Gbps/host) | mean sample goodput | run avg goodput | max ToR queue (MB) |
|---|---:|---:|---:|---:|
| balanced | 60.774 | 52.671 | 11.705 | 1.418 |
| core | 33.996 | 28.595 | 6.354 | 0.803 |
| incast | 46.356 | 37.307 | 8.290 | 1.493 |

### 4.2 facebook_hadoop

| traffic config | max goodput (Gbps/host) | mean sample goodput | run avg goodput | max ToR queue (MB) |
|---|---:|---:|---:|---:|
| balanced | 55.444 | 43.732 | 9.718 | 0.345 |
| core | 55.477 | 43.960 | 9.769 | 0.312 |
| incast | 50.683 | 40.118 | 8.915 | 1.417 |

### 4.3 web_search

| traffic config | max goodput (Gbps/host) | mean sample goodput | run avg goodput | max ToR queue (MB) |
|---|---:|---:|---:|---:|
| balanced | 73.452 | 41.465 | 9.214 | 0.190 |
| core | 71.040 | 41.839 | 9.298 | 0.244 |
| incast | 72.605 | 40.680 | 9.040 | 1.224 |

## 5. 结果怎么看

### 5.1 core 会压低吞吐

`google_rpc` 在 `core` 下的峰值 goodput 从 `60.774` 降到 `33.996` Gbps/host，下降很明显。说明共享核心链路被收紧后，吞吐确实被压住了。

### 5.2 incast 会抬高 ToR queue

三类 workload 里，`incast` 的 ToR queue 峰值普遍更高，尤其是：

- `google_rpc`: `1.493 MB`
- `facebook_hadoop`: `1.417 MB`
- `web_search`: `1.224 MB`

这和接收端汇聚排队的语义一致。

### 5.3 facebook_hadoop 更稳

`facebook_hadoop` 在 `balanced` 和 `core` 下的吞吐很接近，说明这个 workload 在当前小规模拓扑里没有像 `google_rpc` 那样对核心瓶颈特别敏感；但一到 `incast`，queue 就明显上去了。

### 5.4 web_search 吞吐高，但 queue 仍能被 incast 推起来

`web_search` 的吞吐峰值整体最高，但 `incast` 仍然把 ToR queue 推到 `1.224 MB`。这说明高吞吐不等于没有排队，瓶颈形态换了，排队位置也会变。

## 6. 能不能写进论文

可以这样写场景：

> 我们在一个 32 主机、4 ToR、2 spine 的 leaf-spine 小规模拓扑上，对 `balanced`、`core` 和 `incast` 三类场景进行纸面窗口实验，并对 `google_rpc`、`facebook_hadoop`、`web_search` 三类 workload 分别统计 request-only goodput 与 ToR egress queue 峰值。

可以这样写结论：

> 结果表明，当前实现已经能够稳定输出论文所需的核心指标，并且在不同瓶颈类型下呈现出符合预期的趋势：`core` 会明显压低部分 workload 的 goodput，而 `incast` 会显著抬高接收端方向的 ToR queue 峰值。

## 7. 需要保留的谨慎表述

这份结果适合说成：

- 本地验证
- 纸面窗口对齐
- 趋势对齐

不适合直接说成：

- 严格复现原文大规模 sim1

因为拓扑规模已经缩小了。

