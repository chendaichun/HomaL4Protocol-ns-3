# Lab1 Full Run Conclusion - 2026-04-16

Source traces:

- `/mnt/nasDisk_ds3617/sird/HomaL4Protocol-lab1-receiver-congestion`
- `/mnt/nasDisk_ds3617/sird/output-f/lab1/lab1_summary.csv`

Run status:

- All five cases completed with `started=1500` and `finished=1500`.
- All `queue-drops.tr` files were empty.

FCT summary:

| case | p50 | p99 | mean |
|---|---:|---:|---:|
| 8B unloaded | 2.008 us | 2.008 us | 2.008 us |
| 8B incast SRPT | 6.430 us | 10.816 us | 6.095 us |
| 500KB unloaded | 732.248 us | 732.248 us | 732.248 us |
| 500KB incast SRPT | 2703.890 us | 5220.048 us | 2557.202 us |
| 500KB incast SRR | 3358.255 us | 10708.745 us | 3804.355 us |

Interpretation:

- Incast increases 8B short-message latency. The 8B p50 rises from 2.008 us to 6.430 us, and p99 rises from 2.008 us to 10.816 us.
- For 500KB messages in this receiver-congestion setup, SRPT outperforms SRR. SRR has higher p50, p99, and mean latency.
- Receiver-side qdisc queue occupancy increases under incast. Observed peak queue occupancies were 11 packets for 8B incast, 53 packets for 500KB SRPT, and 199 packets for 500KB SRR.
- These queue traces are receiver downlink qdisc occupancy, not a direct measurement of shared switch/core buffer occupancy.

Do not claim from this run:

- Do not claim SRR reduces 500KB latency; this run shows the opposite.
- Do not claim no queueing delay just because no drops were observed.
- Do not equate the receiver qdisc queue plot with the paper's shared-buffer/core-buffering result.
- Do not compare 8B SRR versus SRPT; this run does not include an 8B SRR case.
