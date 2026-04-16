# Lab1 Run Log

Lab1 is the receiver-congestion/incast microbenchmark:
6 long-message senders send 10MB messages to one receiver, while one probe sender sends either 8B or 500KB messages to the same receiver.

Current code assumptions:

- Link delay in `scratch/lab1.cc`: 1us.
- Device queue and SirdQueueDisc are configured by script/env vars.
- `HomaSendScheduler::MAX_N_MSG = 2560`.
- `scripts/lab1.sh full` currently means `durationSec=0.03`, unless `DURATION_SEC_FULL` is overridden.
- `scripts/lab1.sh fast` currently means `durationSec=0.005`, unless `DURATION_SEC` is overridden.
- Unless stated otherwise, `traceSwitchEgressQueue=0`.

NAS result root:

```text
/mnt/nasDisk_ds3617/sird
```

## Runs

| Result directory | Status | Main parameters | Purpose / observation |
|---|---:|---|---|
| `lab1_full_0p03_8b1us_500kb50us_no_switchq_20260416_235022` | failed | full, 8B=1us, 500KB=50us, queue=1000p, pre-fix grant logic | `incast_500KB_srr` aborted on invalid grant offset. Do not use for conclusions. |
| `lab1_full_0p03_8b1us_500kb50us_no_switchq_fixgrant_20260416_235347` | complete | full, 8B=1us, 500KB=50us, queue=1000p, `MAX_N_MSG=255` | `500KB_srr` had `started=397 finished=142 incomplete=255`; direct evidence of the 255 outstanding-message cap. |
| `lab1_full_0p03_8b1us_500kb50us_no_switchq_fixgrant_maxmsg2560_20260417_002907` | complete | full, 8B=1us, 500KB=50us, queue=1000p, `MAX_N_MSG=2560` | `500KB_srr` improved to `started=600 finished=507 incomplete=93`; message-id cap fixed, but 1000p queue still leaves unfinished SRR messages. |
| `lab1_full_0p03_8b1us_500kb50us_no_switchq_fixgrant_maxmsg2560_q10000p_20260417_003725` | complete | full, 8B=1us, 500KB=50us, queue=10000p, `MAX_N_MSG=2560` | `500KB_srr` completed `600/600`; confirms 1000p was too small for this high-pressure SRR case. 10MB background still had some unfinished messages. |
| `lab1_full_default_8b50us_500kb500us_0p3_no_switchq_fixgrant_20260417_000033` | complete | `durationSec=0.3`, 8B=50us, 500KB=500us, queue=1000p, `MAX_N_MSG=255` binary from before rebuild | `500KB_srr` had `started=600 finished=474 incomplete=126`; not a 255 cap case because all 600 probe messages began. |
| `lab1_tiny_10mb2round_8b50us_500kb500us_q1000p_maxmsg2560_20260417_005848` | complete | fast, `durationSec=0.005`, 8B=50us, 500KB=500us, queue=1000p, `MAX_N_MSG=2560` | About two 10MB rounds per long sender. `500KB_srr` had `10/8`, while `500KB_srpt` completed `10/10`; 1000p can still fail SRR even in a tiny run. |
| `lab1_full_8b40us_500kb2000us_q1000p_maxmsg2560_20260417_011035` | complete | full, 8B=40us, 500KB=2000us, queue=1000p, `MAX_N_MSG=2560` | Lower 500KB probe pressure. `500KB_srr` had `started=15 finished=11 incomplete=4`; `500KB_srpt` completed `15/15`. |
| `lab1_full_8b50us_500kb500us_q1000p_tracequeue_maxmsg2560_20260417_011401` | running | full, 8B=50us, 500KB=500us, queue=1000p, `MAX_N_MSG=2560`, `traceSwitchEgressQueue=1`, sample=1us | Current run requested to collect switch egress queue trace. |
| `lab1_full_8b50us_500kb500us_q1000p_maxmsg2560_bdp16p6_20260417_015049` | running | full, 8B=50us, 500KB=500us, queue=1000p, `MAX_N_MSG=2560`, `bdpPkts=16.6`, `traceSwitchEgressQueue=0` | Re-run after unifying `lab1.cc`/`lab1.sh` around one-way `bdpPkts`; thresholds now derive to `RttPackets=34`, credit budget `25`, unscheduled threshold `17`, sender CSN threshold `8`, mark threshold `21p`. |

Older scratch/result directories:

- `lab1-debug`: debugging output; not part of the current comparison set.
- `lab1-proof`: proof/debug output; not part of the current comparison set.

## Current Interpretation

- The invalid grant-offset crash was a protocol implementation boundary bug. The receiver must not emit `grantOffset == msgSizePkts`.
- The 255 cap explained the first `500KB_srr` failure under 8B=1us, 500KB=50us, queue=1000p.
- After increasing `MAX_N_MSG` to 2560, the remaining incomplete `500KB_srr` cases are consistent with queue/drop/recovery limitations rather than txMsgId exhaustion.
- Raising queue depth from 1000p to 10000p made the high-pressure `500KB_srr` probe workload complete `600/600`.
- FCT plots and `lab1_summary.csv` only count the requested probe sizes (`8` and `500000` bytes), not 10MB background messages.
