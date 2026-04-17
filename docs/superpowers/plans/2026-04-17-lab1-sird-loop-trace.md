# Lab1 SIRD Loop Trace Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a lab1-only trace and plotting path that exposes per-sender CE/CSN control-loop behavior through `netBudget`, `hostBudget`, and windowed CE/CSN counts.

**Architecture:** Extend `HomaL4Protocol` with a dedicated `SirdLoopState` trace source emitted by the receiver scheduler whenever sender control state changes or a grant decision is evaluated. Hook that trace into `scratch/lab1.cc`, then extend `scripts/lab1_plot.py` to generate one CE-loop plot and one CSN-loop plot per sender from the new trace file.

**Tech Stack:** ns-3 C++, existing `TracedCallback` infrastructure, Python 3, matplotlib.

---

### Task 1: Protocol Trace Source

**Files:**
- Modify: `src/internet/model/homa-l4-protocol.h`
- Modify: `src/internet/model/homa-l4-protocol.cc`

- [ ] Add a new `SirdLoopState` trace source and a matching `TraceSirdLoopState(...)` helper on `HomaL4Protocol`.
- [ ] Track per-sender cumulative `DATA`, `CE`, and `CSN` observations in `HomaRecvScheduler`.
- [ ] Emit one `SirdLoopState` sample after reclaim events and one after each SIRD sender-budget update in `SendAppropriateGrants()`.

### Task 2: Lab1 Trace File

**Files:**
- Modify: `scratch/lab1.cc`
- Modify: `scripts/lab1.sh`

- [ ] Add a `traceSirdLoop` switch to `lab1`.
- [ ] Write `lab1_<tag>.sird-loop.tr` with all fields needed by plotting.
- [ ] Expose the switch through `TRACE_SIRD_LOOP` in `scripts/lab1.sh`.

### Task 3: Plotting

**Files:**
- Modify: `scripts/lab1_plot.py`

- [ ] Parse `lab1_<tag>.sird-loop.tr`.
- [ ] Group samples by sender.
- [ ] Generate `CE loop` plots with `netBudget` and windowed `CE` count.
- [ ] Generate `CSN loop` plots with `hostBudget` and windowed `CSN` count.

### Task 4: Verification

**Files:**
- Verify: `scripts/lab1_plot.py`
- Verify: `scratch/lab1.cc`
- Verify: `src/internet/model/homa-l4-protocol.cc`

- [ ] Run a Python syntax check for `scripts/lab1_plot.py`.
- [ ] Review the emitted field names and trace hookup paths for consistency across protocol, `lab1`, and plotting.
