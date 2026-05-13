# 400G Switch Scenario Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Extend the platform so the 5-node 400Gbps single-switch all-to-all scenario can run with shared-buffer ECN, PFC-style pause/resume, QCN-style sender feedback, and per-flow initial rates.

**Architecture:** Add a reusable switch queue-disc and controller for congestion detection and flow control, add a sender-side rate-controlled flow application, then assemble the exact 12-flow 400G scenario in a dedicated scratch program and verify it with focused tests plus a smoke run.

**Tech Stack:** ns-3 C++, traffic-control queue discs, NetDeviceQueueInterface flow control, applications module, scratch scenario, Markdown docs

---

### Task 1: Add a failing queue-disc test for ECN and shared-buffer bookkeeping

**Files:**
- Create: `src/traffic-control/test/switch-shared-buffer-queue-disc-test-suite.cc`
- Modify: `src/traffic-control/wscript`

- [ ] **Step 1: Write the failing test**

Create a test suite skeleton with one test that expects a new queue disc to:
- accept a configured shared-buffer size,
- report queue occupancy,
- mark packets only after occupancy crosses `Kmin`,
- keep packets enqueued until `MaxSize` is exceeded.

- [ ] **Step 2: Run test to verify it fails**

Run: `./waf --run "test-runner --suite=switch-shared-buffer-queue-disc"`  
Expected: build or type failure because `SwitchSharedBufferQueueDisc` does not exist yet.

- [ ] **Step 3: Write minimal queue-disc header/source skeleton**

Create:
- `src/traffic-control/model/switch-shared-buffer-queue-disc.h`
- `src/traffic-control/model/switch-shared-buffer-queue-disc.cc`

Minimal behavior:
- derive from `QueueDisc`,
- expose `MaxSize`, `SharedBufferSize`, `Kmin`, `Kmax`, `Pmax`, `UseEcn`,
- enqueue/dequeue using one internal FIFO,
- mark packets according to occupancy thresholds.

- [ ] **Step 4: Run test to verify it passes**

Run: `./waf --run "test-runner --suite=switch-shared-buffer-queue-disc"`  
Expected: PASS for the first ECN/shared-buffer occupancy case.

- [ ] **Step 5: Commit**

Run:
```bash
git add src/traffic-control/model/switch-shared-buffer-queue-disc.h src/traffic-control/model/switch-shared-buffer-queue-disc.cc src/traffic-control/test/switch-shared-buffer-queue-disc-test-suite.cc src/traffic-control/wscript
git commit -m "traffic-control: add switch shared-buffer queue disc skeleton"
```

### Task 2: Add failing tests for PFC pause/resume signaling

**Files:**
- Modify: `src/traffic-control/test/switch-shared-buffer-queue-disc-test-suite.cc`
- Modify: `src/traffic-control/model/switch-shared-buffer-queue-disc.h`
- Modify: `src/traffic-control/model/switch-shared-buffer-queue-disc.cc`

- [ ] **Step 1: Write the failing test**

Add a test that expects:
- queue occupancy above `PauseThreshold` emits a pause event for the ingress,
- queue occupancy below `ResumeThreshold` emits a resume event.

- [ ] **Step 2: Run test to verify it fails**

Run: `./waf --run "test-runner --suite=switch-shared-buffer-queue-disc"`  
Expected: FAIL because pause/resume trace or callback is not implemented.

- [ ] **Step 3: Implement minimal pause/resume signaling**

Add:
- per-ingress pause state,
- configurable pause/resume thresholds,
- trace sources or callbacks for pause/resume transitions.

- [ ] **Step 4: Run test to verify it passes**

Run: `./waf --run "test-runner --suite=switch-shared-buffer-queue-disc"`  
Expected: PASS for pause/resume transition coverage.

- [ ] **Step 5: Commit**

Run:
```bash
git add src/traffic-control/model/switch-shared-buffer-queue-disc.h src/traffic-control/model/switch-shared-buffer-queue-disc.cc src/traffic-control/test/switch-shared-buffer-queue-disc-test-suite.cc
git commit -m "traffic-control: add PFC-style pause and resume signaling"
```

### Task 3: Add failing tests for sender-side rate control

**Files:**
- Create: `src/applications/test/rate-controlled-flow-app-test-suite.cc`
- Modify: `src/applications/wscript`

- [ ] **Step 1: Write the failing test**

Add a test suite that expects a new application to:
- start at a configured initial rate,
- reduce rate on feedback,
- stop sending while paused,
- resume sending after pause.

- [ ] **Step 2: Run test to verify it fails**

Run: `./waf --run "test-runner --suite=rate-controlled-flow-app"`  
Expected: build failure because `RateControlledFlowApp` does not exist yet.

- [ ] **Step 3: Write minimal application skeleton**

Create:
- `src/applications/model/rate-controlled-flow-app.h`
- `src/applications/model/rate-controlled-flow-app.cc`

Minimal behavior:
- one sender app bound to one destination,
- `PacketSize`, `InitialRate`, `MaxBytes`,
- timer-driven packet sends,
- methods to apply pause/resume and multiplicative rate decrease.

- [ ] **Step 4: Run test to verify it passes**

Run: `./waf --run "test-runner --suite=rate-controlled-flow-app"`  
Expected: PASS for the initial control semantics.

- [ ] **Step 5: Commit**

Run:
```bash
git add src/applications/model/rate-controlled-flow-app.h src/applications/model/rate-controlled-flow-app.cc src/applications/test/rate-controlled-flow-app-test-suite.cc src/applications/wscript
git commit -m "applications: add rate-controlled flow app"
```

### Task 4: Bridge queue-disc events into sender control

**Files:**
- Create: `src/traffic-control/model/switch-congestion-controller.h`
- Create: `src/traffic-control/model/switch-congestion-controller.cc`
- Modify: `src/traffic-control/wscript`

- [ ] **Step 1: Write the failing test**

Extend the queue-disc test or add a controller-level test that expects:
- a pause event to stop the mapped sender,
- a resume event to wake it,
- a QCN event to reduce the sender rate.

- [ ] **Step 2: Run test to verify it fails**

Run: `./waf --run "test-runner --suite=switch-shared-buffer-queue-disc"`  
Expected: FAIL because there is no controller wiring.

- [ ] **Step 3: Implement minimal controller**

Implement:
- registration of ingress or flow IDs to sender app instances,
- callbacks for pause/resume/QCN events,
- direct invocation of app control methods.

- [ ] **Step 4: Run test to verify it passes**

Run: `./waf --run "test-runner --suite=switch-shared-buffer-queue-disc"`  
Expected: PASS for queue-disc-to-sender control wiring.

- [ ] **Step 5: Commit**

Run:
```bash
git add src/traffic-control/model/switch-congestion-controller.h src/traffic-control/model/switch-congestion-controller.cc src/traffic-control/wscript
git commit -m "traffic-control: add switch congestion controller"
```

### Task 5: Assemble the exact 400G scenario

**Files:**
- Create: `scratch/qcn_400g_alltoall.cc`

- [ ] **Step 1: Write the failing smoke scenario**

Create a scratch program that:
- builds nodes `0..4`,
- connects `0-1`, `0-2`, `0-3`, `0-4`,
- configures `400Gbps`, `4us`, `4000B payload`,
- instantiates the 12 flows from the provided matrix.

- [ ] **Step 2: Run build to verify it fails or is incomplete**

Run: `./waf build --run scratch/qcn_400g_alltoall`  
Expected: failure until all new module APIs are wired correctly.

- [ ] **Step 3: Wire full scenario behavior**

Add:
- `82MB` shared buffer,
- `KMIN=700`, `KMAX=1600`, `PMAX=0.2`,
- per-flow initial rates:
  - `2->1=4Gbps`, `3->1=4Gbps`, `4->1=400Gbps`
  - `1->2=200Gbps`, `3->2=200Gbps`, `4->2=4Gbps`
  - `1->3=200Gbps`, `2->3=200Gbps`, `4->3=4Gbps`
  - `1->4=4Gbps`, `2->4=200Gbps`, `3->4=200Gbps`
- start time `40us`,
- flow size `100000000B`,
- stop time `0.5s`.

- [ ] **Step 4: Run smoke test to verify it passes**

Run: `./waf --run "qcn_400g_alltoall --stopTime=0.01"`  
Expected: scenario starts, installs all 12 flows, and exits without assertion or build failure.

- [ ] **Step 5: Commit**

Run:
```bash
git add scratch/qcn_400g_alltoall.cc
git commit -m "scratch: add 400G single-switch all-to-all scenario"
```

### Task 6: Add trace outputs and write the scenario report

**Files:**
- Modify: `scratch/qcn_400g_alltoall.cc`
- Create: `docs/qcn_400g_alltoall_paper_writeup_zh.md`

- [ ] **Step 1: Write failing expectations for outputs**

Decide and verify the scenario emits:
- flow completion records,
- sender rate timeline,
- queue occupancy timeline,
- ECN/PFC/QCN event counters.

- [ ] **Step 2: Run smoke test to verify output gaps**

Run: `./waf --run "qcn_400g_alltoall --stopTime=0.01 --outputDir=outputs/qcn-smoke"`  
Expected: some output files missing before instrumentation is complete.

- [ ] **Step 3: Add instrumentation and report**

Instrument the scenario and write a Chinese Markdown report describing:
- scenario topology,
- parameter meanings,
- what each output file represents,
- how to use the plots in the paper.

- [ ] **Step 4: Run smoke test to verify outputs exist**

Run: `./waf --run "qcn_400g_alltoall --stopTime=0.01 --outputDir=outputs/qcn-smoke"`  
Expected: output files are generated and non-empty.

- [ ] **Step 5: Commit**

Run:
```bash
git add scratch/qcn_400g_alltoall.cc docs/qcn_400g_alltoall_paper_writeup_zh.md
git commit -m "docs: add 400G switch scenario report"
```
