# sim1 Paper Baseline And Comparison Method

This file records the paper-derived baseline used to judge `scratch/sim1.cc`.

Source: SIRD NSDI 2025 paper, Figure 5 and Table 5. Table 5 gives raw values for:

- 99th percentile slowdown of all messages at 50% offered application load.
- Maximum goodput across applied load levels, in Gbps.
- Maximum ToR queuing across applied load levels, in MB.

Only the SIRD row is directly relevant for the current sim1 SIRD reproduction.

## Paper-Aligned Parameters

Use these unless intentionally running a sensitivity experiment:

```text
BDPBytes=100000
payloadSizeBytes=1442
RttPackets=69
SirdCreditBudgetPkts=104
SirdUnschThresholdPkts=69
SirdSenderCsnThresholdPkts=35
qdiscMarkThreshold=87p
```

Workload mapping:

```text
WKa / google_rpc      -> inputs/W3_Google_AllRPC_cdf.csv
WKb / facebook_hadoop -> inputs/W4_Facebook_Hadoop_cdf.csv
WKc / web_search      -> inputs/W5_DCTCP_scaled1442_bytes_cdf.csv
```

Traffic configuration mapping:

```text
Default in paper -> balanced in sim1 scripts
Core             -> core
Incast           -> incast
```

## Paper Raw Values For SIRD

### 99th Percentile Slowdown At 50% Load

| config | WKa | WKb | WKc |
|---|---:|---:|---:|
| Default/Balanced | 1.70 | 3.53 | 3.99 |
| Core | 1.37 | 2.28 | 2.54 |
| Incast | 1.65 | 3.76 | 5.79 |

Paper SIRD mean: `2.96`, range: `4.42`.

### Maximum Goodput Across Applied Load Levels, Gbps

| config | WKa | WKb | WKc |
|---|---:|---:|---:|
| Default/Balanced | 79.74 | 82.27 | 84.71 |
| Core | 48.27 | 50.47 | 48.75 |
| Incast | 79.70 | 81.98 | 84.13 |

Paper SIRD mean: `71.11`, range: `36.44`.

Important: this is **maximum across a load sweep**, not the goodput from one run at `OFFERED_LOAD=0.5`.

### Maximum ToR Queuing Across Applied Load Levels, MB

| config | WKa | WKb | WKc |
|---|---:|---:|---:|
| Default/Balanced | 0.76 | 0.81 | 0.75 |
| Core | 2.26 | 1.67 | 1.39 |
| Incast | 0.79 | 0.81 | 0.83 |

Paper SIRD mean: `1.12`, range: `1.51`.

Important: this is **maximum across a load sweep**.

## How To Compare sim1 Outputs

For every `(trafficConfig, workload)` case, compare with the matching table cell above:

```text
trafficConfig=balanced, workload=google_rpc      -> Default/Balanced WKa
trafficConfig=balanced, workload=facebook_hadoop -> Default/Balanced WKb
trafficConfig=balanced, workload=web_search      -> Default/Balanced WKc
trafficConfig=core,     workload=google_rpc      -> Core WKa
trafficConfig=core,     workload=facebook_hadoop -> Core WKb
trafficConfig=core,     workload=web_search      -> Core WKc
trafficConfig=incast,   workload=google_rpc      -> Incast WKa
trafficConfig=incast,   workload=facebook_hadoop -> Incast WKb
trafficConfig=incast,   workload=web_search      -> Incast WKc
```

Trace interpretation:

- `sim1_<tag>.goodput.tr`: application goodput sampled by completed message bytes.
- `sim1_<tag>.tor-egress-queue.tr`: sampled ToR egress queue occupancy.
- `sim1_<tag>.msg.tr`: message begin/finish trace used for slowdown.

Metric extraction:

- Goodput: take max sampled `goodputGbps` per run. To reproduce Table 5, run a load sweep and take max over loads.
- ToR queue: convert queue bytes to MB and take the max over all sampled ToR egress queues and time samples. To reproduce Table 5, run a load sweep and take max over loads.
- 99p slowdown: match `+` and `-` lines in `msg.tr`, compute message FCT, divide by the chosen minimum possible FCT model, then take p99 over completed messages. For paper Figure 5/Table 5, use all messages at 50% offered load.

Current caveat:

The repo has trace generation for all three inputs, but a final `sim1_analyze.py` that emits a paper-style CSV from these traces may still need to be added or verified before claiming exact Table 5 reproduction.
