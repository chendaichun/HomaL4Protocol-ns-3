# BOLT Swift Mode Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Wire up `BoltL4Protocol::CC_SWIFT` so it can be selected explicitly, verified with a minimal smoke, and compared against default BOLT behavior in a small correctness experiment.

**Architecture:** Reuse the existing BOLT transport stack and treat Swift as a congestion-control mode inside `BoltL4Protocol`, not a separate protocol. Keep the change surface small: add explicit cc-mode selection and reporting in smoke tooling, then add one standalone dumbbell experiment that runs once with `DEFAULT` and once with `SWIFT` and emits enough signal to confirm the two modes diverge.

**Tech Stack:** ns-3 C++, `./waf` build/run, existing `BoltL4Protocol`, `BoltSocketFactory`, `PfifoBoltQueueDisc`.

---

## File Structure

- Modify: `src/internet/model/bolt-l4-protocol.h`
  - Confirm public enum/string mapping and add any tiny helper needed to stringify the active cc mode without changing transport behavior.
- Modify: `src/internet/model/bolt-l4-protocol.cc`
  - Keep `CcMode` attribute authoritative; add a small helper implementation only if needed for observability in experiments.
- Modify: `scratch/bolt-smoke.cc`
  - Accept `ccMode` as a command-line option, set the global default before stack installation, and print the active mode in the final status line.
- Create: `scratch/bolt-swift-compare.cc`
  - Minimal single-bottleneck experiment that can run in either `DEFAULT` or `SWIFT` mode and reports received bytes plus queue samples.
- Optional Create: `scripts/bolt_swift_compare.sh`
  - Thin runner that invokes the new scratch twice, once per mode, and stores outputs in separate directories. Skip this if the scratch command line is already concise enough.

### Task 1: Make cc-mode selection explicit in the smoke

**Files:**
- Modify: `scratch/bolt-smoke.cc`
- Modify: `src/internet/model/bolt-l4-protocol.h`
- Modify: `src/internet/model/bolt-l4-protocol.cc`

- [ ] **Step 1: Write the failing smoke invocation for Swift**

Run:

```bash
./waf --run "bolt-smoke --ccMode=SWIFT"
```

Expected: FAIL because `bolt-smoke` does not yet accept `--ccMode`.

- [ ] **Step 2: Add a tiny string helper for cc-mode reporting**

In `src/internet/model/bolt-l4-protocol.h`, add a public helper declaration near `GetCcMode`:

```cpp
  enum CcMode_e GetCcMode(void) const;
  static std::string CcModeToString (enum CcMode_e ccMode);
```

In `src/internet/model/bolt-l4-protocol.cc`, implement it next to `GetCcMode`:

```cpp
std::string
BoltL4Protocol::CcModeToString (enum BoltL4Protocol::CcMode_e ccMode)
{
  switch (ccMode)
    {
    case CC_DEFAULT:
      return "DEFAULT";
    case CC_SWIFT:
      return "SWIFT";
    default:
      return "UNKNOWN";
    }
}
```

- [ ] **Step 3: Teach `bolt-smoke` to accept and apply `ccMode`**

In `scratch/bolt-smoke.cc`, add the new argument, parse it, and apply it before `InternetStackHelper::Install`:

```cpp
  std::string ccMode = "DEFAULT";

  CommandLine cmd;
  cmd.AddValue ("ccMode", "Bolt congestion control mode: DEFAULT or SWIFT", ccMode);
  cmd.AddValue ("simDuration", "Simulation duration in seconds", simDuration);
  ...
  cmd.Parse (argc, argv);

  NS_ABORT_MSG_IF (ccMode != "DEFAULT" && ccMode != "SWIFT",
                   "Unsupported ccMode, use DEFAULT or SWIFT");

  Config::SetDefault ("ns3::BoltL4Protocol::CcMode", StringValue (ccMode));
```

Also include the active mode in the final print:

```cpp
  std::cout << "BOLT_SMOKE cc_mode=" << ccMode
            << " total_rx_bytes=" << g_totalRxBytes
            << " expected=" << msgSizeBytes << std::endl;
```

- [ ] **Step 4: Rebuild and verify both modes pass**

Run:

```bash
./waf build
./waf --run "bolt-smoke --ccMode=DEFAULT"
./waf --run "bolt-smoke --ccMode=SWIFT"
```

Expected:

```text
BOLT_SMOKE cc_mode=DEFAULT total_rx_bytes=65536 expected=65536
BOLT_SMOKE cc_mode=SWIFT total_rx_bytes=65536 expected=65536
```

- [ ] **Step 5: Commit**

```bash
git add src/internet/model/bolt-l4-protocol.h \
        src/internet/model/bolt-l4-protocol.cc \
        scratch/bolt-smoke.cc
git commit -m "feat: expose bolt swift mode in smoke test"
```

### Task 2: Add a minimal BOLT-vs-SWIFT correctness experiment

**Files:**
- Create: `scratch/bolt-swift-compare.cc`
- Test: `scratch/bolt-swift-compare.cc`

- [ ] **Step 1: Write the failing experiment invocation**

Run:

```bash
./waf --run "bolt-swift-compare --ccMode=DEFAULT"
```

Expected: FAIL because the scratch file does not exist yet.

- [ ] **Step 2: Implement a dumbbell experiment with queue sampling**

Create `scratch/bolt-swift-compare.cc` with a focused topology:

```cpp
/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */

#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/internet-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/traffic-control-module.h"

using namespace ns3;

namespace {

uint64_t g_leftRx = 0;
uint64_t g_rightRx = 0;
std::ofstream g_queueOut;

void
ReceiveInto (Ptr<Socket> socket, uint64_t *counter)
{
  Address from;
  while (Ptr<Packet> packet = socket->RecvFrom (from))
    {
      *counter += packet->GetSize ();
    }
}

void
SendMany (Ptr<Socket> socket, const Address &peer, uint32_t msgSizeBytes, uint32_t count)
{
  for (uint32_t i = 0; i < count; ++i)
    {
      Ptr<Packet> packet = Create<Packet> (msgSizeBytes);
      int sent = socket->SendTo (packet, 0, peer);
      NS_ABORT_MSG_IF (sent < 0, "bolt-swift-compare send failed");
    }
}

void
SampleQueue (Ptr<QueueDisc> qdisc)
{
  g_queueOut << Simulator::Now ().GetSeconds () << ","
             << qdisc->GetCurrentSize ().GetValue () << std::endl;
  Simulator::Schedule (MicroSeconds (100), &SampleQueue, qdisc);
}

} // namespace

int
main (int argc, char *argv[])
{
  std::string ccMode = "DEFAULT";
  std::string queueTrace = "bolt-swift-queue.csv";
  double simDuration = 0.02;
  uint32_t msgSizeBytes = 64 * 1024;
  uint32_t flowCount = 32;

  CommandLine cmd;
  cmd.AddValue ("ccMode", "Bolt congestion control mode: DEFAULT or SWIFT", ccMode);
  cmd.AddValue ("queueTrace", "Queue trace CSV path", queueTrace);
  cmd.AddValue ("simDuration", "Simulation duration in seconds", simDuration);
  cmd.AddValue ("msgSizeBytes", "Per-message bytes", msgSizeBytes);
  cmd.AddValue ("flowCount", "Messages per sender", flowCount);
  cmd.Parse (argc, argv);

  Config::SetDefault ("ns3::BoltL4Protocol::CcMode", StringValue (ccMode));

  NodeContainer leftSenders, rightSenders, switchNode, receivers;
  leftSenders.Create (1);
  rightSenders.Create (1);
  switchNode.Create (1);
  receivers.Create (2);

  PointToPointHelper edge;
  edge.SetDeviceAttribute ("DataRate", StringValue ("10Gbps"));
  edge.SetChannelAttribute ("Delay", StringValue ("2us"));
  edge.SetDeviceAttribute ("Mtu", UintegerValue (1500));

  PointToPointHelper bottleneck;
  bottleneck.SetDeviceAttribute ("DataRate", StringValue ("1Gbps"));
  bottleneck.SetChannelAttribute ("Delay", StringValue ("10us"));
  bottleneck.SetDeviceAttribute ("Mtu", UintegerValue (1500));

  NetDeviceContainer leftEdge = edge.Install (leftSenders.Get (0), switchNode.Get (0));
  NetDeviceContainer rightEdge = edge.Install (rightSenders.Get (0), switchNode.Get (0));
  NetDeviceContainer downLeft = edge.Install (switchNode.Get (0), receivers.Get (0));
  NetDeviceContainer downRight = bottleneck.Install (switchNode.Get (0), receivers.Get (1));

  InternetStackHelper internet;
  NodeContainer all;
  all.Add (leftSenders);
  all.Add (rightSenders);
  all.Add (switchNode);
  all.Add (receivers);
  internet.Install (all);

  TrafficControlHelper boltQdisc;
  boltQdisc.SetRootQueueDisc ("ns3::PfifoBoltQueueDisc",
                              "MaxSize", StringValue ("1000p"),
                              "EnableBts", BooleanValue (true),
                              "EnableTrimming", BooleanValue (true),
                              "EnablePru", BooleanValue (true));
  QueueDiscContainer qdiscs = boltQdisc.Install (downRight);

  ...

  std::cout << "BOLT_SWIFT_COMPARE cc_mode=" << ccMode
            << " left_rx=" << g_leftRx
            << " right_rx=" << g_rightRx << std::endl;
}
```

Implementation notes:
- Keep the bottleneck only on one downlink so queue behavior is easy to see.
- Use two senders and two receivers so both sockets are active but topology stays small.
- Sample only the bottleneck queue disc to CSV.

- [ ] **Step 3: Verify the experiment runs in both modes**

Run:

```bash
./waf --run "bolt-swift-compare --ccMode=DEFAULT --queueTrace=/tmp/bolt-default-queue.csv"
./waf --run "bolt-swift-compare --ccMode=SWIFT --queueTrace=/tmp/bolt-swift-queue.csv"
```

Expected:
- Both runs exit `0`.
- Both print a `BOLT_SWIFT_COMPARE` status line.
- Both CSV files exist and contain multiple lines.

- [ ] **Step 4: Check that the two modes are behaviorally different**

Run:

```bash
wc -l /tmp/bolt-default-queue.csv /tmp/bolt-swift-queue.csv
tail -n 5 /tmp/bolt-default-queue.csv
tail -n 5 /tmp/bolt-swift-queue.csv
```

Expected:
- Both files have non-trivial sample counts.
- Final queue samples are not byte-for-byte identical across modes.
- If they are identical, stop and inspect whether `CcMode` is actually being read on the active `BoltL4Protocol` instance.

- [ ] **Step 5: Commit**

```bash
git add scratch/bolt-swift-compare.cc
git commit -m "feat: add bolt versus swift correctness experiment"
```

### Task 3: Add one reproducible runner or verification note for future use

**Files:**
- Create: `scripts/bolt_swift_compare.sh`

- [ ] **Step 1: Write the failing runner invocation**

Run:

```bash
bash scripts/bolt_swift_compare.sh
```

Expected: FAIL because the script does not exist yet.

- [ ] **Step 2: Add a thin runner that stores both modes side by side**

Create `scripts/bolt_swift_compare.sh`:

```bash
#!/bin/zsh
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
TS="${TS:-$(date +%Y%m%d_%H%M%S)}"
OUT_DIR="${OUT_DIR:-$ROOT_DIR/outputs/bolt-swift-compare-$TS}"
mkdir -p "$OUT_DIR"

cd "$ROOT_DIR"
./waf --run "bolt-swift-compare --ccMode=DEFAULT --queueTrace=$OUT_DIR/default-queue.csv" \
  | tee "$OUT_DIR/default.log"
./waf --run "bolt-swift-compare --ccMode=SWIFT --queueTrace=$OUT_DIR/swift-queue.csv" \
  | tee "$OUT_DIR/swift.log"
```

- [ ] **Step 3: Run the script and confirm both outputs land in one directory**

Run:

```bash
bash scripts/bolt_swift_compare.sh
find outputs -maxdepth 1 -type d -name 'bolt-swift-compare-*' | tail -n 1
```

Expected:
- One fresh output directory.
- Inside it: `default.log`, `swift.log`, `default-queue.csv`, `swift-queue.csv`.

- [ ] **Step 4: Final verification**

Run:

```bash
./waf build
./waf --run "bolt-smoke --ccMode=DEFAULT"
./waf --run "bolt-smoke --ccMode=SWIFT"
bash scripts/bolt_swift_compare.sh
```

Expected:
- Build succeeds.
- Both smokes pass.
- Compare runner completes and produces paired outputs.

- [ ] **Step 5: Commit**

```bash
git add scripts/bolt_swift_compare.sh
git commit -m "chore: add bolt swift comparison runner"
```

## Self-Review

- Spec coverage:
  - Swift mode wired up explicitly: covered by Task 1.
  - Minimal smoke for Swift: covered by Task 1.
  - One experiment that demonstrates BOLT/Swift migration correctness: covered by Task 2 and Task 3.
- Placeholder scan:
  - No `TODO`/`TBD` placeholders remain.
  - Every task includes exact files and commands.
- Type consistency:
  - `ccMode` is always `DEFAULT` or `SWIFT`.
  - The helper name `CcModeToString` is used consistently.
  - The comparison scratch is consistently named `bolt-swift-compare`.

## Execution Handoff

Plan complete and saved to `docs/superpowers/plans/2026-04-22-swift-bolt-mode.md`. Two execution options:

**1. Subagent-Driven (recommended)** - I dispatch a fresh subagent per task, review between tasks, fast iteration

**2. Inline Execution** - Execute tasks in this session using executing-plans, batch execution with checkpoints

Which approach?
