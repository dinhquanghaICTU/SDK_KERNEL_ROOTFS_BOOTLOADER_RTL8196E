# MEMO — RTL8196E TX throughput, session 2026-05-02

## Context

Session driven by the brief
`~/Documents/RTL8196E_Docs/BRIEF-tx-throughput-orthogonal-levers.md`,
exploring four orthogonal levers to push TCP TX past the 70.8 Mbit/s
ceiling measured in May 2026.

Hardware: Lidl Silvercrest Gateway, RTL8196E (Lexra RLX4181 @ 380 MHz,
32 MB DDR, 8 KB writeback non-coherent D-cache + 16 KB IRAM).
Linux 6.18.24 + driver `rtl8196e-eth` v2.4.  Working branch:
`feat/tx-throughput` based on main `f1b4808`.

## Consolidated measurements (5 × 60 s × 4 workloads, median Mbit/s)

| Workload | R₀ baseline | A (kick coalescing) | B+ (writeback only) | C (NAPI W=128) | D (full SG) |
|---|---:|---:|---:|---:|---:|
| TCP RX | 93.5 | 93.4 | 93.3 | 93.3 | 93.3 |
| **TCP TX** | **69.3** | **70.1** ✓ | 69.3 | 69.5 | 69.3 |
| UDP TX 100M | 37.9 | 37.9 | 37.6 | 37.5 | 37.0 |
| UDP storm 64B | 1.88 | 1.87 | 1.87 | 1.90 | 1.84 |

Intra-phase variance ≈ 1 %.  Significance threshold = 2σ ≈ 2 %.

## Verdict per track

### Track A — `kick_tx` coalescing (threshold N=4)

**Kept**, merged to main.  Δ TCP TX = **+1.2 %** (at the variance edge but
positive and consistent across 5 reps: 71.2 / 69.9 / 70.2 / 69.9 / 70.1
→ median 70.1 vs R₀ 69.3).  No regression elsewhere.  Mechanism: pulse
`TXFD` on `CPUICR` at most once per 4 submits (except cold-start
`was_empty == true`), drained at the end of every NAPI poll.  Saves
~3 µs of MMIO bus time per 4-packet batch.

### Track B+ — TX flush writeback-only (skip invalidate)

**Rejected**, reverted.  Δ TCP TX vs A = **−1.1 %**.  Causal hypothesis:
on this hardware with an 8 KB D-cache, keeping a 1500 B buffer warm
(~19 % of the cache) evicts useful lines (sockets, NAPI ctx) — the
invalidate frees the cache better than warm-keeping.  The current
`dma_cache_wback_inv` stays optimal here.

Useful side note for archive: the wiring of `_dma_cache_wback` to
`rlx_dma_cache_wback_inv` in `c-lexra.c:300` is latent (kernel API
asks for writeback-only but Lexra resolves to wback+inv).  No effect
today (no `dma_cache_wback` caller in the tree) but worth fixing if a
new caller appears.

### Track C — NAPI weight 64 → 128

**Rejected**, reverted.  Δ TCP TX vs A = **−0.9 %** (consistent, σ ~0.3,
~2.5σ).  Causal hypothesis: on a single-core CPU, a larger weight
starves process context (start_xmit syscall) in favour of NAPI poll.
The default 64 is well matched to this hardware.

### Track D — TX scatter-gather

**HW probe positive**, **full SG implementation rejected**.

`rtl8196e_ring_tx_sg_test` confirmed that the **switch ASIC honours
mBuf `m_next` chains on TX**, despite no usage by the Realtek BSP and
the misleading `mbuf.h` comment ("MBUF_EOR is set only by ASIC", true
on RX only).  A 96-byte 2-mBuf chain reaches the wire intact (ethertype
0x88B5 broadcast, payload `DEADBEEF` + incrementing pattern crossing
the mb1↔mb2 boundary at offset 32).

The full SG implementation (`tx_submit_sg`, 512-tail mBuf pool in
KSEG1, frag walk, `NETIF_F_SG`) was coded and benched.  Setting
`NETIF_F_SG` flips the kernel from 0 % to **99.96 %** non-linear SKBs
on TCP TX (it points directly at user pages via frags instead of
linearising).  But Δ TCP TX vs A = **−1.1 %**.  Hypothesis: on this
slow CPU, splitting one big 1500 B flush into N small flushes
(head + frags) costs more than skipping `skb_linearize` (a 1500 B
memcpy on hot cache is very cheap).

Reverted to keep main close to the baseline + Track A.

## Strategic synthesis

The brief promised 5–15 % per track.  Measured reality: **±1.5 % noise
on all four tracks**, with only A net-positive at the threshold.  The
real TX bottleneck is not in the driver hot path (audited line by
line via three ktime_get probes) but in:

- the TCP/IP send-side stack (~80–90 µs of CPU per packet out of
  ~132 µs of total "sys" time)
- the DDR memory bus (1500 B writebacks = 1.4 µs ≈ 84 % of cache flush
  time)
- the absence of useful hardware instructions (no MIPS-IV+ `pref`, no
  FPU, no `lwl/lwr/swl/swr` — RLX4181 is strict MIPS-1)

The theoretical TCP TX ceiling on this SoC at 380 MHz under the
current conditions is **~71 Mbit/s** (single-core saturated 99 %
sys+sirq, no TX HW offload).  Going higher would require a lever
not listed in the brief: jumbo frames, hardware TSO/GSO (absent on
this switch), or a faster SoC.

## What is kept on main (v3.4.1)

- Track A coalescing (`rtl8196e_ring_kick_tx(ring, was_empty)` with
  `pending_kicks` counter, threshold via `rtl8196e_kick_threshold`,
  drain via `rtl8196e_ring_kick_drain` called at the end of every
  `napi_poll`).

## What stays on the `feat/tx-throughput` archive branch

- Probe instrumentation `xmit/kick/cache` (sysfs, default-off).
- Bench harness `bench_tx.sh`.
- Scatter-gather test `rtl8196e_ring_tx_sg_test` + sysfs `tx_sg_test`.
- Full SG implementation (`tx_submit_sg` + tail pool) reverted at tip
  but archived in history for future reuse.
- All raw bench output (5 directories `tx_<phase>_<timestamp>/`).

Branch retained as a session archive.

## Brief criteria

- Minimal (≥1 track with a measurable verdict): **achieved** (Track A +1.2 %).
- Good (TCP TX ≥ 75 Mbit/s): **not achieved** (70.1).
- Excellent (TCP TX ≥ 80 Mbit/s): **not achieved** (70.1).

Measured ceiling on this SoC ≈ **71 Mbit/s** under iperf2 single-stream
conditions.
