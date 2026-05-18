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

5 findings total. TMR-001..004 fixed in driver `1.0` (v3.4.0).
TMR-005 lands in v3.4.2 as a coordinated rework with the watchdog
driver (WDT-005 closure) — the clocksource side of a shared-CDBR fix.

| ID | Type | Severity | Confidence | Status | One-liner |
|----|------|----------|------------|--------|-----------|
| TMR-001 | ROBUSTNESS / PLATFORM | medium | probable | **fixed** | clock divider not validated; `clk_prepare_enable(busclk)` return ignored |
| TMR-002 | ROBUSTNESS / API | medium | probable | **fixed** | `clockevents_config_and_register()` ran before `request_irq()` — IP7 storm risk |
| TMR-003 | ROBUSTNESS / PLATFORM | low | probable | **fixed** | IRQ handler returned `IRQ_HANDLED` unconditionally, never `IRQ_NONE` |
| TMR-004 | API / ROBUSTNESS | low | certain | **fixed** | `clocksource_register_hz()` return value ignored |
| TMR-005 | PLATFORM / FUNCTIONAL | medium | certain | **applied (v3.4.2)** | shared-CDBR DivFactor reworked 8→8000 via new 25 kHz `slowclk` DT node so the on-chip watchdog has a useful overflow window (WDT-005 closure) |

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

## Applied — v3.4.2 (no driver version bump)

### TMR-005 — slowclk CDBR rework for the watchdog

The on-chip watchdog (`drivers/watchdog/rtl819x_wdt.c`) shares the
CDBR clock (sysc + 0x3118) with Timer0/Timer1, and at the v3.4.0
DivFactor=8 setting its OVSEL=1001 max bucket overflows in only
~671 ms — too tight for a BusyBox userspace feeder. The full
diagnosis lives in `drivers/watchdog/AUDIT.md` (WDT-005).

The clocksource side of the fix:

* A new 25 kHz `slowclk` fixed-clock DT node feeds the timer's
  `clocks[0]`. The driver reads it as `timer_rate`, computes
  `div_fac = bus_rate / timer_rate = 200 MHz / 25 kHz = 8000`, and
  writes the CDBR register accordingly. This is exactly the
  DivFactor the Realtek SDK BSP programs.
* The watchdog inherits the new 25 kHz tick automatically (shared
  CDBR), so OVSEL=1001 overflow grows ~671 ms → ~671 s. WDT-005
  closes.
* `clockevents` `min_delta` reduced 0x300 (768 ticks) → 8. At
  25 MHz the old 0x300 represented ~31 µs; at 25 kHz it would have
  represented 30 ms — incompatible with HZ=250 (4 ms periodic
  tick). 8 ticks at 25 kHz = 320 µs, comfortable below the HZ
  interval but well above the read/write/arm latency on this
  200 MHz Lexra MIPS.

Trade-off — `sched_clock` granularity drops from 40 ns to 40 µs.
Visible in perf/ftrace precision and dmesg `printk` timestamp
precision; invisible to kernel timekeeping (HZ=250 = 4 ms tick).
The instrumented build's softlockup detector still works correctly
because `watchdog_thresh` is in seconds, not nanoseconds.

Improvements that come for free at 25 kHz:

* TC1 28-bit clocksource wrap grows 10.7 s → ~3 h, so
  `clocksource_watchdog`'s wrap-handling has 1000× less work.
* TMR-001's clock-divider bounds check (`!div_fac || div_fac > 0xffff`)
  still holds: 8000 < 65535 = 0xffff.

No driver-source change is required for the rate change itself —
all values flow from the new `clock-frequency` in DT. The only C
edit is the `min_delta` value in the `clockevents_config_and_register`
call.

The watchdog driver bumps to 1.1 to mark the regime change. The
clocksource driver does not bump; its responsibilities are
unchanged and the audited-fix list (TMR-001..004) still applies.

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
