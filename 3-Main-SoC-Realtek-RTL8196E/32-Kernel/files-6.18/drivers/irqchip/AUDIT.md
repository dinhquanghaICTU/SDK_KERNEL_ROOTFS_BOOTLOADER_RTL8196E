# RTL8196E INTC driver — robustness / API / perf audit

Target: Linux 6.18 port of the `irq-rtl819x` interrupt controller driver
for the Realtek RTL8196E SoC. Manages the 32-bit GIMR/GISR registers,
programs IRR routing for peripheral → CPU IP lines, exposes an
irqdomain of 32 hwirq, installs chained handlers on the parent CPU IP
lines.

Audit date: 2026-05-01. Driver version at audit time: pre-`1.0`
(unversioned) → bumped to `1.0` as part of this pass.

The driver is short (~390 lines) and already carefully scoped:
single-port MMIO, no DMA, no buffers. Audit focus was on init
ordering, lifecycle alignment with the modern irqchip lifecycle
(`.irq_unmask` semantics), DT vs hardcoded parent IRQ assumptions, and
hot-path duplicate work. Plus one perf tuning specific to the gateway's
actual workload.

## Summary of findings

7 findings total. 3 fixed in driver `1.0`, 1 perf tuning applied
(IRR1 swap), 3 rejected after analysis.

| ID | Type | Severity | Confidence | Status | One-liner |
|----|------|----------|------------|--------|-----------|
| IRQ-001 | ROBUSTNESS / PERF | high | probable | **fixed** | GIMR enabled all child sources globally at init, before consumers requested |
| IRQ-002 | ROBUSTNESS / PLATFORM | high | hypothesis | **rejected** | TC0 dual-routed (IRR1 + GIMR + direct IP7) — actually required by HW (see analysis) |
| IRQ-003 | API / ROBUSTNESS | medium | certain | **fixed** | DT declared one parent IRQ but driver chained on three (IP2/IP3/IP4) |
| IRQ-004 | ROBUSTNESS / PERF | medium | probable | **fixed** | GISR ack happened twice per IRQ (parent-side W1C + `.irq_ack` via level flow) |
| IRQ-005 | API / ROBUSTNESS | medium | probable | **deferred** | `irq_domain_create_legacy` with fixed base 16 — works, no measured gain to migrating |
| IRQ-006 | ROBUSTNESS / PLATFORM | low | certain | **deferred** | hardcoded source bits 12/13/15 not validated against DT — current DT matches |
| IRQ-007 | PLATFORM / PERF | low | certain | **deferred** | virq cache without `READ_ONCE/WRITE_ONCE` — single-core only, no race |

Plus one **perf tuning**, not from the audit but from the post-fix
analysis of `plat_irq_dispatch()` priority ordering for the actual
gateway workload:

| ID | Type | Status | One-liner |
|----|------|--------|-----------|
| PERF-UART1-IRR | PERF | **applied** | swap UART1↔Switch IRR routing so UART1 sits on IP4 (higher priority than IP3 Switch) |

## Applied fixes — driver 1.0

All commits on `private/main` between `3912e3f..0b9405a`. Detailed
mapping:

### IRQ-001 — only arm TC0 in GIMR at init; let .irq_unmask enable the rest

Commit `3912e3f`. The previous init wrote GIMR = `BIT(TC0) | BIT(UART0) |
BIT(UART1) | BIT(SW_CORE)` before any consumer driver had registered a
handler. The chained dispatcher could then receive sources whose virq
was not yet mapped and would only log via `pr_warn_ratelimited`.

For UART0 / UART1 / Switch the fix is to rely on the standard
`.irq_unmask` path: `serial8250_register_8250_port` and `rtl8196e-eth`'s
`request_irq()` both trigger `realtek_soc_irq_unmask()` which sets the
GIMR bit at the right moment.

**TC0 has to stay unconditional.** The timer DT node is parented to
`&cpuintc/<7>` and the timer driver requests CPU IRQ 7 directly, so it
never traverses this irqdomain's `.irq_unmask`. Yet the only hardware
path from TC0 to a CPU IP is via INTC IRR1 + GIMR — verified against
the open-source bootloader (`31-Bootloader/boot/monitor.c:163,190`
routing TC0 to IP4 via IRR1, `irq.c:39,142-153` arming GIMR bit 8 in
its `request_IRQ()`). Clearing GIMR bit 8 here would silence IP7 and
hang the kernel at `clocksource_init`. **This is also why IRQ-002 is
rejected** — see below.

### IRQ-003 — describe and parse IP2/IP3/IP4 parent IRQs from DT

Commit `cc8ce9d`. The `intc@3000` node previously declared a single
parent IRQ (`interrupts = <2>`) but the driver hardcoded
`irq_set_chained_handler_and_data()` against the constants
`REALTEK_CPU_IRQ_CASCADE/UART1/SWITCH` (2/3/4). The DT was therefore
lying about which CPU IPs the controller actually chains on, and the C
side silently depended on the cpuintc legacy domain numbering.

Move the parent IRQ list into the DT (`interrupts = <2>, <3>, <4>;
interrupt-names = "cascade", "uart1", "switch";` — names later updated
to `"cascade", "switch", "uart1"` after the IRR1 swap below) and resolve
them at runtime with `irq_of_parse_and_map()`. The driver no longer
references the CPU IP numbers; the `REALTEK_CPU_IRQ_*` constants are
gone.

### IRQ-004 — drop redundant GISR ack in chained handler

Commit `28893f1`. The chained dispatcher was W1C-clearing the
per-source GISR pending bit before calling `generic_handle_irq(virq)`.
The child IRQ is wired to `handle_level_irq`, which then runs
`realtek_soc_irq_ack()` on the same bit through the standard flow. That
made every IRQ cost two identical MMIO writes to GISR.

Drop the parent ack and rely on the child `.irq_ack` via the level
flow handler. Sources stay correct: GISR mirrors the per-source latch
and clears when the peripheral handler drains its hardware (the stock
bootloader follows the same pattern in `monitor.c:128` — `TC_IR` is
W1C'd without ever touching GISR).

### Version bump 1.0 + boot banner

Commit `9f3bf39`. Added `DRV_VERSION "1.0"` and updated the boot
`pr_info()` to display the version + the IP routing summary.

## Perf tuning — UART1 / Switch IRR1 swap

Commit `0b9405a`. Not from the audit, but applied in the same window
because the IRR1 routing is naturally re-examined when the audit
exposes the parent IRQ topology.

`plat_irq_dispatch()` services pending MIPS IPs in fixed order
IP7 > IP4 > IP3 > IP2. Previously UART1 sat on IP3 and the Ethernet
switch on IP4, so under simultaneous activity the Ethernet ISR would
preempt the UART1 ISR. For this gateway the priority should be
inverted:

* **UART1** carries the Zigbee link to the EFR32 radio. The 8250 has a
  16-byte RX FIFO; at 460800 baud that is roughly 350 µs of latency
  budget. An overrun drops a Zigbee frame and forces Z2M / ZHA to
  reconnect — visible to the user.
* **Ethernet** uses DMA descriptor rings plus NAPI, so a delayed switch
  IRQ at most translates to a TCP retransmit — invisible.

Move UART1 to IP4 and Switch to IP3 by swapping the corresponding
nibbles in IRR1, update the DT `interrupt-names` accordingly, and
refresh the boot banner. The driver itself is mapping-agnostic since
IRQ-003 made it parse parents from the DT, so no driver logic change.

Validated by the overnight OTBR 460800 soak: 8h+ stable, zero
`HandleRcpTimeout`, ttyS1 LSR shows `THRE | TEMT` only (no overrun bit
set on the periodic sampler).

## Rejected after analysis

### IRQ-002 — TC0 dual-routed (IRR1 + GIMR + direct IP7)

The audit posited that TC0 might have a hardware path direct to IP7
independent of the INTC, in which case `IRR1[3:0]=0x7` + `GIMR bit 8
set` would be redundant. Examined the open-source bootloader source
(`31-Bootloader/boot/monitor.c:163,190` and `irq.c`): the bootloader
**explicitly** routes TC0 to IP4 via IRR1 and arms GIMR bit 8 via
`unmask_irq()`. **No direct TC0→IP7 path exists in hardware.** The
kernel's current setup (route to IP7 via IRR1, leave GIMR bit 8 set,
have the timer driver request CPU IRQ 7 directly via `&cpuintc`)
is the only way the timer can fire. The kernel's chained INTC handler
on IP7 is intentionally absent (we only chain on IP2/IP3/IP4) so there
is no double-dispatch — GISR bit 8 clears naturally when the timer
driver W1Cs `TC_IR` (the bootloader confirms: it never touches GISR in
its `timer_interrupt`).

Conclusion: applying the proposed change would silence IP7 → kernel
hang at `clocksource_init`. Rejected.

### IRQ-005 — `irq_domain_create_legacy` → `irq_domain_create_linear`

Functionally valid migration, no measured gain (DT consumers go through
`irq_of_parse_and_map`, no global numbering dependency). The legacy
domain works, costs nothing. Deferred.

### IRQ-006 — DT validation of source bits

Current DT matches the hardcoded `REALTEK_HW_*_BIT` constants. A future
DT change would simply not work, which is detectable. No silent
mis-binding risk on this codebase. Deferred.

### IRQ-007 — `READ_ONCE/WRITE_ONCE` on virq cache

Optimisation for the SMP case which this platform is not. Single-core
RLX4181 — no race possible on these reads. Deferred.

## Validation

* Boot banner `irq-rtl819x v1.0 (J. Nilo) - Timer:IP7, UART1:IP4,
  Switch:IP3, UART0:IP2`.
* `/proc/interrupts` shows ttyS1 on IRQ 29 / hwirq 13 (IP4) and eth on
  IRQ 31 / hwirq 15 (IP3) after the swap — confirms IRR1 took effect.
* `ERR: 0` on the irq controller line (no spurious).
* Overnight soak: 8h+ OT-RCP at 460800 baud, two paired Sleepy End
  Devices, zero overruns measured by the periodic LSR sampler.

## How this maps to the public release

Driver `1.0` plus the IRR1 perf swap ship in **v3.4.0**.
