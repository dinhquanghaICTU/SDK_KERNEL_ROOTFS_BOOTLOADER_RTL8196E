# RTL8196E Ethernet driver — Security / robustness / perf audit

Target: Linux 6.18 port of the `rtl8196e-eth` driver for the Realtek
RTL8196E SoC (Lexra RLX4181, single-core MIPS-I BE, non-coherent
writeback L1, 32-byte cache lines, no LWL/LWR, no hardware divide).

Audit date: 2026-04-23. Driver version at audit time: `2.2`.
Second-pass audit: 2026-05-01 (driver `2.3` → `2.4`); see end of file.
Baseline throughput on the Lidl Silvercrest gateway: **~94 Mbit/s RX,
~71 Mbit/s TX**.

The driver was already carefully written: `__iram` placement for hot
paths, NAPI with GRO defer tuning, KSEG1 (uncached) rings, explicit
`dma_cache_*` discipline for non-coherent DMA, single-producer /
single-consumer TX ring with `READ_ONCE` / `WRITE_ONCE`. The audit did
**not** find an exploitable security vulnerability or a certain memory
corruption in nominal operation. What follows is focused on the
remaining rough edges and their fixes.

## Summary of findings

17 findings total. 10 were fixed in **batch 1**; 1 (F8) was applied as
a follow-up refactor; 1 (F6) was tested and rejected; 3 (F11/F13/F15)
were bundled, tested and rejected (major regression); 3 are
informational and intentionally left as-is.

| ID | Type | Severity | Confidence | Status | One-liner |
|----|------|----------|------------|--------|-----------|
| F1 | ROBUSTNESS | high | certain | **fixed** | `tx_timeout` didn't synchronise with NAPI → ring corruption race |
| F2 | ROBUSTNESS | high | certain | **fixed** | MAC change in UP was silently broken (NETIF / L2 not reprogrammed) |
| F3 | PLATFORM | high | certain | **fixed** | 650 ms of `mdelay` in process context during `ndo_open` |
| F4 | ROBUSTNESS | high | certain | **fixed** | Poll-ready loops used `udelay` where `usleep_range` is schedulable |
| F5 | ROBUSTNESS | medium | certain | **fixed** | RX drop / bad-length paths didn't update `rx_errors` / `rx_dropped` |
| F6 | PERF | medium | probable | **tested, rejected** | Descriptor pools in KSEG1: TCP TX -1.2 Mb/s, gain only on UDP bidir |
| F7 | API | medium | certain | **fixed** | ISR acked bits then returned `IRQ_NONE` (spurious-count risk) |
| F8 | ROBUSTNESS | medium | certain | **fixed** | `device_create_file` replaced by `attribute_group` via `sysfs_groups[]` |
| F9 | ROBUSTNESS | medium | probable | **fixed** | `kick_tx` bypassed the `readl/writel` helpers |
| F10 | ROBUSTNESS | medium | probable | **fixed** | `table_write` proceeded when `tlu_start` timed out |
| F11 | PERF | low | probable | **tested, rejected** | bundled with F13/F15 — major regression on hardware |
| F12 | ROBUSTNESS | low | certain | **fixed** | `mb = ph->ph_mbuf` dereferenced without pool bound check |
| F13 | PERF | low | probable | **tested, rejected** | bundled with F11/F15 — major regression on hardware |
| F14 | ROBUSTNESS | low | certain | **fixed** | `stop()` didn't W1C latched status bits in `CPUIISR` |
| F15 | API | low | certain | **tested, rejected** | bundled with F11/F13 — major regression on hardware |
| F16 | STYLE | info | certain | intentional | `(void)hw` in functions that never use `hw` (vestigial API) |
| F17 | PLATFORM | info | hypothesis | documented | HW DMA registers are programmed with KSEG1 virtual addresses |

## Batch 1 — applied fixes

All changes live in the three C files of this directory.

### F1 — synchronise NAPI around `tx_timeout`

File: `rtl8196e_main.c` in `rtl8196e_tx_timeout()`.

Why: in 6.x, `ndo_tx_timeout` runs in the watchdog workqueue (process
context). NAPI poll runs in softirq and can also be driven by the GRO
hrtimer (`gro_flush_timeout = 2 ms`), so masking the hardware IRQ is
no longer enough to freeze NAPI. While `tx_reset()` `memset`s the
descriptor pool and resets `tx_cons = 0`, NAPI could be concurrently
reading `tx_ring[tx_cons]` and dereferencing the `mb`/skb behind it →
use-after-free or index desynchronisation. Rare (needs a TX timeout
during live traffic) but certain to exist.

```diff
 	netif_stop_queue(ndev);
+	napi_disable(&priv->napi);
 	rtl8196e_hw_disable_irqs(&priv->hw);
 	rtl8196e_hw_stop(&priv->hw);
 	rtl8196e_ring_tx_reclaim(priv->ring, &pkts, &bytes, 0);
 	rtl8196e_ring_tx_reset(priv->ring);
 	rtl8196e_hw_set_tx_ring(&priv->hw, rtl8196e_ring_tx_desc_base(priv->ring));
 	rtl8196e_hw_start(&priv->hw);
+	napi_enable(&priv->napi);
 	rtl8196e_hw_enable_irqs(&priv->hw);
 	netif_wake_queue(ndev);
```

### F2 — refuse MAC change while the interface is UP

File: `rtl8196e_main.c`, new `rtl8196e_set_mac_address()` replacing the
default `eth_mac_addr` in `rtl8196e_netdev_ops`.

Why: the vanilla `eth_mac_addr` updates `ndev->dev_addr` but does not
touch the hardware NETIF table (48-bit MAC embedded in `word0`/`word1`)
nor the hashed "toCPU" L2 entry. A live `ip link set eth0 address ...`
would silently break all unicast reception. Cleanest minimal fix:
refuse it while UP; the next `open()` reprograms both tables from
`ndev->dev_addr`.

```c
static int rtl8196e_set_mac_address(struct net_device *ndev, void *p)
{
	if (netif_running(ndev))
		return -EBUSY;
	return eth_mac_addr(ndev, p);
}
```

### F3 — `mdelay` → `msleep` in `rtl8196e_hw_init`

File: `rtl8196e_hw.c`. Three occurrences (two `mdelay(300)` + one
`mdelay(50)`).

Why: `hw_init` is called from `ndo_open`, process context. Six hundred
fifty milliseconds of busy-wait on a single-core 400 MHz MIPS blocks
ksoftirqd and every other task during every `ifconfig up`. `msleep` is
schedulable.

### F4 — `udelay(10)` → `usleep_range(10, 20)` in poll-ready loops

File: `rtl8196e_hw.c` in `rtl8196e_mdio_wait_ready`,
`rtl8196e_table_wait_ready`, `rtl8196e_tlu_start`.

Why: each loop bounds to 1000 × 10 µs = 10 ms worst case. Called
repeatedly from `rtl8196e_l2_clear_table` (1024 iterations of
`rtl8196e_l2_write_entry`, each doing two or three of these waits), so
the cumulative worst-case can reach several seconds of busy-wait in
`open()`. Schedulable sleep lets the rest of userland progress under
silicon stress.

### F5 — RX error counters

File: `rtl8196e_ring.c` in `rtl8196e_ring_rx_poll`.

Why: three paths recycled the descriptor silently:

- `rxb->skb == NULL` (defensive — shouldn't happen)
- `napi_alloc_skb()` returned NULL (OOM)
- `ph->ph_len` out of `[ETH_ZLEN, buf_size]`

None of these updated `rx_errors` / `rx_dropped`, so field debug via
`ip -s link` or `ethtool -S` showed nothing. Added a dedicated
`rearm_drop` label for the first two (OOM-ish) and
`rx_errors`+`rx_length_errors` on `rearm_bad`.

### F7 — ISR: mask before W1C

File: `rtl8196e_main.c` in `rtl8196e_isr`.

Why: the old sequence `read → W1C → mask → return IRQ_NONE on zero`
could clear bits we didn't actually process (not armed in CPUIIMR)
before returning `IRQ_NONE`. Kernel treats consistent `IRQ_NONE` as
spurious → disables the IRQ line after enough "unhandled" counts. New
sequence reads both `CPUIISR` and `CPUIIMR`, intersects them, returns
`IRQ_NONE` **without** clearing if nothing is ours, otherwise W1Cs
only our bits.

### F9 — `kick_tx` via the MMIO helpers

File: `rtl8196e_ring.c` in `rtl8196e_ring_kick_tx`.

Why: every other MMIO in the driver goes through `rtl8196e_readl /
writel`. `kick_tx` open-coded `*(volatile u32 *)CPUICR` which is
functionally equivalent today but diverges silently if the helpers are
ever moved behind `ioremap`. Also added a comment on the posting-read
pattern used to make the TXFD pulse visible to hardware.

### F10 — abort `table_write` / `l2_write_entry` on TLU failure

File: `rtl8196e_hw.c` in `rtl8196e_table_write`,
`rtl8196e_l2_write_entry`.

Why: the old code set `tlu_ok = false` and proceeded with the table
write. Depending on silicon revision, writes to `TBL_ACCESS_CTRL`
without the Table Lookup Unit engaged may not be latched into the ASIC
RAM, giving silently-empty entries with no verification for VLAN /
NETIF / broadcast rows. Now returns `-EIO` with a rate-limited warning
and lets the caller decide (`open()` already has a fallback path via
`rtl8196e_hw_l2_trap_enable`).

### F12 — bound-check `ph->ph_mbuf` in RX poll

File: `rtl8196e_ring.c` in `rtl8196e_ring_rx_poll`.

Why: after the CPU invalidates the pkthdr, `mb = ph->ph_mbuf` reads a
value that hardware has been able to touch. The driver only ever
assigns `ph_mbuf` to valid pool pointers at ring creation, but a
silicon bug or DRAM corruption could plant a wild pointer → oops on
dereference. Added a pool-bound check with fallback to the
index-derived mapping (`ring->rx_mbuf_base[rx_idx]`) on failure.
Defense in depth; expected to be a no-op in the field.

### F14 — clear CPUIISR on `stop()`

File: `rtl8196e_main.c` in `rtl8196e_stop`.

Why: after disabling IRQs and stopping hardware, `CPUIISR` can still
carry latched status bits (unacked RX_DONE, runout). A subsequent
`open()` starts with stale status. One `writel(readl(CPUIISR),
CPUIISR)` at shutdown guarantees a clean slate.

## Tested and rejected

### F6 — descriptor pools in KSEG1 (perf, experimental)

**Status: tested on hardware 2026-04-23, rejected.** Branch
`audit-batch2-f6` (now deleted) implemented this on top of batch 1,
with a one-shot `dma_cache_wback_inv` over each pool before aliasing
it to KSEG1 via `rtl8196e_uncached_addr`. All per-descriptor
`dma_cache_*` calls in `tx_submit` / `tx_reclaim` / `rx_poll` /
`tx_reset` / `dbg_timer_fn` were removed since `ph`, `mb` and their
derived pointers ended up in KSEG1 too. Kernel built and booted
cleanly, IRQ 31 `spurious` stayed at 0.

Hypothesis was that trading ~4 per-packet cache ops for uncached
loads/stores on small descriptors would win on hot paths. Measurement
says otherwise:

| Test | Batch 1 | F6 | Δ |
|------|---------|----|---|
| TCP Ubuntu→RTL (RX) | 93.9 Mb/s | 93.9 Mb/s | = |
| **TCP RTL→Ubuntu (TX)** | **72.2 Mb/s** | **71.0 Mb/s** | **−1.2** |
| TCP Parallel ×4 | 95.0 Mb/s | 95.3 Mb/s | +0.3 |
| TCP Parallel ×8 | 96.1 Mb/s | 95.5 Mb/s | −0.6 |
| TCP stress 300 s | 93.9 Mb/s | 94.0 Mb/s | +0.1 |
| UDP 10M / 50M | 10.5 / 52.4, 0% | 10.5 / 52.4, 0% | = |
| UDP 100M (CPU-bound) | 42.3, 56% loss | 42.3, 56% loss | = |
| **UDP bidir to-rtl** | **29.4 Mb/s, 44% loss** | **31.5 Mb/s, 40% loss** | **+2.1** |
| UDP bidir from-rtl | 13.2 Mb/s, 0% | 13.2 Mb/s, 0% | = |

TCP TX regresses by −1.2 Mb/s (≈1.7 %), above the 1 Mb/s "worth
investigating" noise floor from `32-Kernel/CLAUDE.md`. Likely cause:
`tx_submit` does ~10 stores to `ph`/`mb` fields. In KSEG0 these hit
the cache (cheap) and get batched by a single `dma_cache_wback_inv`.
In KSEG1 each store is a direct bus access, which beats the single
cache op only when the descriptor is touched once or twice — not
when it is touched ten times. Conversely, RX-dominant workloads win,
hence the +2.1 Mb/s on UDP bidir.

Decision: net loss on the production workload (TCP TX already being
the slower direction). Not merged.

If this is revisited, try a hybrid: keep the pools in KSEG0 but
touch them via a KSEG1 alias only for the handover word (`tx_ring`
entry, which is already KSEG1), avoiding both the full per-descriptor
wback on TX and the cache ops during reclaim/poll. Or: batch
descriptor fields so the store count drops (currently `tx_submit`
writes `ph_len`, `ph_vlanId`, `ph_portlist`, `ph_srcExtPortNum`,
`ph_flags` separately, with bit-field RMWs on two of them).

### F11 + F13 + F15 — descriptor-ring micro-optimisations (bundled)

**Status: tested on hardware 2026-04-23, rejected.** Branch
`audit-micro-opts` (now deleted) implemented the three items as a
single commit on top of F8 (main at `5eaca40`). Kernel built and
booted fine, IRQ 31 `spurious` stayed at 0, but the iperf suite
regressed massively on the very first tests:

| Test | Batch 1 baseline | F11+F13+F15 | Δ |
|------|------------------|-------------|---|
| TCP Ubuntu→RTL (RX) | 93.9 Mb/s | 46.5 Mb/s | **−47 Mb/s (−50 %)** |
| TCP RTL→Ubuntu (TX) | 72.2 Mb/s | 49.4 Mb/s | **−23 Mb/s (−30 %)** |

The suite was interrupted after the second test — the regression is
obvious and the remaining tests were skipped.

Likely culprits (not bisected — commit was reverted in one step):

- **F13** (`wback_inv → inv` on the RX rearm buffer). In theory
  equivalent on clean cache lines and semantically safe because HW is
  about to overwrite the buffer, but Lexra cache behaviour may differ
  from the assumption. Possible interaction with the cached data lines
  that hold `mb`/`ph` in the same region, or a subtle timing effect on
  the `hit_invalidate_d` instruction when lines are still held by
  a previous stack consumer.
- **F15** (compute WRAP from the ring index instead of RMW). The
  transformation looks algebraically equivalent — initial WRAP bits
  live only on the last slot of each ring, the driver never moves them
  — but the original RMW may have a side effect we are not modelling
  (e.g. ordering guarantee through the uncached read) that the straight
  write does not provide.
- **F11** (remove `dma_cache_inv` on a KSEG1 tx_ring entry). On paper a
  no-op; unlikely to cause regression by itself, but could amplify
  reordering if paired with F15's change to the following store.

Decision: keep batch 1 + F8 as the shipping configuration. Revisit
micro-opts only individually, each in its own branch with the full iperf
suite as gate. The expected gain per item is small (under the noise
floor of the baseline) so the upside does not justify the risk of
another bundle-level regression.

If revisited, start with F11 alone (lowest risk, least interesting
gain), then F13 alone with special attention to per-test retransmission
rate, then F15 alone with a double-check of the WRAP-bit transition
under back-to-back submit+reclaim (e.g. instrument the descriptor state
via the debug timer).

## Deferred items

_None remaining. All 17 findings have been either applied, rejected
after testing, or documented as intentionally left as-is._

## Non-issues verified

- **`napi_alloc_skb` reserves `NET_SKB_PAD + NET_IP_ALIGN` in 6.18**
  (verified in `net/core/skbuff.c:804,845`). The RX rearm path does
  preserve IP alignment; the existing code comment is correct.
- **TX submission memory ordering**: `wback → wmb → handover → wmb` is
  correct; both barriers surround the ownership flip.
- **TX concurrency**: `start_xmit` is serialised by the netdev queue
  lock (no `NETIF_F_LLTX`). NAPI only reads `tx_prod` via `READ_ONCE`
  and writes `tx_cons` via `WRITE_ONCE`. Clean SP/SC.
- **Descriptor alignment vs ownership bits**: `sizeof(struct
  rtl_pktHdr) == 20` and `sizeof(struct rtl_mBuf) == 32` — both
  multiples of 4, so bits 0 and 1 of `&pool[i]` are always zero and
  the OR with OWNED / WRAP never corrupts the address.
- **SKB length trust**: `ph->ph_len` is bound-checked to
  `[ETH_ZLEN, buf_size = 1700]`. `skb_put(skb, len)` never overruns.
- **6.x API**: `timer_container_of`, `timer_setup`, `timer_delete_sync`,
  `netif_napi_add` without weight, `of_get_mac_address` new signature,
  `eth_hw_addr_set`, `eth_hw_addr_random` — all correct for 6.x.
- **OF node refcounts in `dt.c`**: `for_each_child_of_node` handles the
  ref; returned node is paired with a `of_node_put` in the caller.
- **Stack usage**: `u32 a[8] + u32 b[8]` in `l2_read_entry` is 64 bytes
  — well within the 8 KB MIPS kernel stack.

## Validation results (batch 1)

Full `scripts/test_rtl8196e_eth.sh` suite, #4 (pre-batch1) vs
#5 (post-batch1):

| Test | Before | After | Δ |
|------|--------|-------|---|
| TCP Ubuntu→RTL (RX) | 93.9 Mb/s, retrans 10 | 93.9 Mb/s, retrans 1 | = / better |
| TCP RTL→Ubuntu (TX) | 71.2 Mb/s, retrans 3 | 72.2 Mb/s, retrans 0 | +1.0 |
| TCP parallel ×4 | 95.4 Mb/s, 0.17 % retx | 95.0 Mb/s, **0.04 % retx** | -0.4, 4× fewer |
| TCP parallel ×8 | 95.5 Mb/s, 0.13 % retx | 96.1 Mb/s, 0.15 % retx | +0.6 |
| TCP 300 s stress | 94.1 Mb/s, retrans 11 | 93.9 Mb/s, retrans 12 | noise |
| UDP 10 M (non-sat.) | 10.5 Mb/s, 0 loss | 10.5 Mb/s, 0 loss | = |
| UDP 50 M (non-sat.) | 52.4 Mb/s, 0 loss | 52.4 Mb/s, 0 loss | = |
| UDP 100 M (saturating) | 41.4 Mb/s, 57 % loss | 42.3 Mb/s, 56 % loss | CPU-bound |
| UDP bidir to-rtl | 30.9 Mb/s, 41 % loss | 29.4 Mb/s, 44 % loss | noise (saturating) |
| UDP bidir from-rtl | 13.1 Mb/s, 0 loss | 13.2 Mb/s, 0 loss | = |
| `rx_errors` | 0 | 0 | = |
| `tx_errors` | 0 | 0 | = |

Spot checks:

- **F7 / F14**: `/proc/irq/31/spurious` shows `count 0 / unhandled 0 /
  last_unhandled 0 ms` both after the full test suite and after a
  cold reboot.
- **F2**: `ip link set dev eth0 address 02:...` while UP returns
  `ioctl 0x8924 failed: Resource busy` (EBUSY, exit 2). Confirmed on
  hardware.

No regression on any measured metric. TCP TX gained +1 Mb/s and the
4-stream retransmission rate dropped by roughly 4×, consistent with
F7/F14 stabilising the IRQ path.

## How to retest end-to-end

```bash
# Rebuild
cd 3-Main-SoC-Realtek-RTL8196E/32-Kernel/
./build_kernel.sh

# Flash over SSH (gateway already on custom firmware)
../flash_remote.sh -y kernel 192.168.1.88

# Confirm build counter
ssh root@192.168.1.88 'uname -a'

# Regression suite (takes ~10 min)
./scripts/test_rtl8196e_eth.sh "some-label"

# Quick IRQ sanity
ssh root@192.168.1.88 'cat /proc/irq/31/spurious'

# F2 validation — DO NOT run this over SSH on the tested interface;
# use a serial console or a second network path.
#   # (refusal in UP)
#   ip link set dev eth0 address 02:de:ad:be:ef:42   # → -EBUSY
#   # (change in DOWN)
#   ip link set eth0 down
#   ip link set dev eth0 address 02:de:ad:be:ef:42
#   ip link set eth0 up
#   /etc/init.d/S10network restart
```

## Overlay-sync reminder

Sources in this overlay tree are only copied into
`../../linux-6.18-rtl8196e/drivers/net/ethernet/rtl8196e-eth/` on a
fresh extraction. After editing a file here, copy it by hand or the
incremental build silently rebuilds the stale tree:

```bash
cp rtl8196e_{main,hw,ring}.c \
   ../../linux-6.18-rtl8196e/drivers/net/ethernet/rtl8196e-eth/
```

A `./build_kernel.sh -v 6.18 clean` rebuild avoids this at the cost of
a ~5-minute rebuild from scratch.

## Second-pass audit (2026-05-01) — driver 2.3 → 2.4

A second independent audit was run against the same source tree (driver
at `2.3`, post-batch1+F8). It produced 8 findings labelled
`RTL8196E-ETH-001..008`. Cross-referenced against the F1-F17 history
above:

| New ID | Status | Notes |
|--------|--------|-------|
| ETH-001 | **fixed in 2.4** | New finding — short TX frames not zero-padded (`skb_put_padto`). Security. Not covered by F1-F17. |
| ETH-002 | **fixed in 2.4** | F1 had fixed tx_timeout/NAPI sync and F14 had W1C'd CPUIISR on stop, but stop() still did not reset rings. New `rtl8196e_ring_rx_reset()` + both resets called in stop(). |
| ETH-003 | deferred | RX `CHECKSUM_UNNECESSARY` blanket assumption. Audit itself recommends "no patch without HW test of bad-checksum drop semantics". Not addressed in this pass. |
| ETH-004 | **fixed in 2.4** | Added `BUILD_BUG_ON` guards on descriptor sizes / offsets in `rtl8196e_ring.c`. Verified non-issue list above already noted size assertions; this pass formalises them at compile time. |
| ETH-005 | **already documented** | Same as F17 ("HW DMA registers programmed with KSEG1 virtual addresses"). Intentional — no refactor. |
| ETH-006 | **fixed in 2.4** | DT `member-ports` / `untag-ports` now bounded to the 9-port HW window (`0x1ff`) and `untag ⊆ member` enforced. |
| ETH-007 | not fixed | `rtl8196e_debug` is opt-in via module param (off by default). No driver-side change needed. |
| ETH-008 | **already rejected** | Same proposal as F13 (`wback_inv → inv` on RX rearm). F13 was tested on HW 2026-04-23 in the F11+F13+F15 bundle and **rejected** (-47 Mb/s RX, -23 Mb/s TX). Re-test only F13 alone in a dedicated branch with the iperf suite as gate, per the F11+F13+F15 section above. |

Net result: 4 patches (A/C/D/B) shipped in driver `2.4` (commits
`7705b70`, `420fb6c`, `518daae`, `9cbb34c`). 2 findings rejected by
construction (ETH-005, ETH-008 — already covered above), 2 deferred
(ETH-003 needs HW characterisation, ETH-007 is intentional opt-in).

