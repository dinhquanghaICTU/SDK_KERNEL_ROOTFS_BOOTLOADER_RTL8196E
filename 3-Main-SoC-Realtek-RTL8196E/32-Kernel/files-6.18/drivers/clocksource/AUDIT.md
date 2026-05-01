# RTL819x timer driver — robustness / API audit

Target: Linux 6.18 port of the `timer-rtl819x` clocksource / clockevent
driver for the Realtek RTL8196E SoC (Lexra RLX4181, single-core MIPS-I
BE, no CP0 `Count` usable for profiling — this driver is the platform's
only reliable timekeeping source).

Audit date: 2026-05-01. Driver version at audit time: pre-`1.0`
(unversioned) → bumped to `1.0` as part of this pass.

The driver is short (~340 lines) and already carefully scoped:
single-port MMIO, no DMA, no buffers, no IRQ chip — just clocksource
(Timer1 free-running) + clockevent (Timer0 one-shot) bound to CPU IP7
via `&cpuintc`. Audit focus was on init ordering, error propagation,
and IRQ handler correctness — no vulnerability or memory corruption
expected by construction of the driver scope.

## Summary of findings

4 findings total. All 4 fixed in driver `1.0`.

| ID | Type | Severity | Confidence | Status | One-liner |
|----|------|----------|------------|--------|-----------|
| TMR-001 | ROBUSTNESS / PLATFORM | medium | probable | **fixed** | clock divider not validated; `clk_prepare_enable(busclk)` return ignored |
| TMR-002 | ROBUSTNESS / API | medium | probable | **fixed** | `clockevents_config_and_register()` ran before `request_irq()` — IP7 storm risk |
| TMR-003 | ROBUSTNESS / PLATFORM | low | probable | **fixed** | IRQ handler returned `IRQ_HANDLED` unconditionally, never `IRQ_NONE` |
| TMR-004 | API / ROBUSTNESS | low | certain | **fixed** | `clocksource_register_hz()` return value ignored |

## Applied fixes — driver 1.0

All commits on `private/main` between `76d97d1..7543969` (range
inclusive). Detailed mapping:

### TMR-004 — propagate `clocksource_register_hz()` error

Commit `76d97d1`. `rtl819x_clocksource_init()` now returns `int` and
`rtl819x_timer_init()` aborts via `panic()` on failure instead of
silently continuing with `sched_clock_register()` against a clocksource
the kernel rejected.

### TMR-001 — validate clock divider and busclk enable

Commit `d9af44b`. Three checks:

* `clk_prepare_enable(busclk)` return is now checked; on failure, fall
  back to the 200 MHz hardcoded default and skip the rate read.
* `panic()` if `timer_rate > bus_rate` (would yield `div_fac == 0`).
* `panic()` if `div_fac` is 0 or exceeds the 16-bit `CLOCK_DIV[31:16]`
  field width.

In practice the DT pins busclk=200 MHz / refclk=25 MHz so `div_fac=8`
and the bounds never trigger, but a misconfigured DT used to silently
program a bogus divider and break kernel timekeeping.

### TMR-002 — quiesce Timer0 + request_irq before clockevents_config_and_register

Commit `224a8a3`. Reordered the bring-up so the IRQ handler is
installed before the clockevent device is exposed to the core:

1. `rtl819x_clocksource_init(timer_rate)` and check return.
2. Disable Timer0 (`CTRL.TC0_EN = 0`), W1C any stale `TC0_PENDING`
   from bootloader / soft-reset state, mask `IR.TC0_EN`.
3. `request_irq()` — handler is now ready.
4. `clockevents_config_and_register()` — only now is Timer0 visible
   to the core, which may immediately call `set_next_event()` and
   enable the timer.

Previously, clockevents was registered before `request_irq()`, so a
stray or core-driven Timer0 fire could land before any handler existed.
On this platform CPU IP7 is level-triggered and dedicated to Timer0
(see `irq-rtl819x` AUDIT for the routing analysis), so an unhandled
assertion turns into an interrupt storm rather than a single lost edge.

### TMR-003 — return IRQ_NONE when TC0_PENDING is not set

Commit `d696cb7`. Read `REALTEK_TC_REG_IR` first; if the Timer0
pending bit is clear, the IRQ did not come from us — return `IRQ_NONE`
without touching the register. The kernel's spurious-IRQ machinery
can then catch and report a misrouted line, instead of the driver
silently absorbing it.

The acknowledge path is unchanged (write-1-to-clear of `TC0_PENDING`),
just folded into a single `writel`.

### Version bump 1.0 + boot banner

Commit `7543969`. Bring this driver in line with the other custom
RTL8196E drivers (`rtl8196e-eth`, `8250_rtl819x`, `rtl8196e-uart-bridge`,
later `irq-rtl819x` and `gpio-rtl819x`) by introducing an internal
`DRV_VERSION` and tagging the boot `pr_info()` with it. Started at
`1.0` covering the post-audit baseline (TMR-001..004).

No `MODULE_VERSION`: this driver is built-in only via
`TIMER_OF_DECLARE`, not loadable as a module.

## Validation

* Build asserts hold (panic conditions are dead code with the actual
  DT, but compile cleanly).
* `dmesg | grep timer-rtl819x` shows the new banner with the actual
  CLK rate (25.000 MHz on the production DT).
* `date +%s; sleep 60; date +%s` reports a 60s delta (within the SSH
  round-trip overhead).
* `/proc/interrupts` IP7 increments at the expected rate (~270 IRQ/s
  with `NO_HZ_IDLE`).
* Soak: 8h+ OTBR-RCP at 460800 baud with no kernel timer-related warn,
  zero `clocksource_watchdog` complaints.

## Non-issues verified

* Not a netdev; no NAPI, RX, TX, DMA, rings, descriptors, skb.
* No buffer manipulation in or out; no overflow surface.
* MMIO via `readl`/`writel` (proper barriers), 32-bit aligned offsets
  only.
* `cpumask_of(0)` is correct for this single-core platform.
* No suspend/resume, no remove path — appropriate for a
  `TIMER_OF_DECLARE` early-init driver.

## How this maps to the public release

Driver `1.0` ships in **v3.4.0** (`3-Main-SoC-Realtek-RTL8196E`
CHANGELOG entry, 2026-05-01). All 4 fixes are in the cumulative
post-v3.3.0 commit window.
