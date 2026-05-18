# RTL8196E watchdog driver — design audit

Target: Linux 6.18 hardware-watchdog driver `rtl819x-wdt` for the
Realtek RTL8196E SoC. Drives the single 32-bit Watchdog Timer Control
Register (WDTCNR) at sysc + 0x311C, registers a system restart
handler so that `reboot` falls through the same path, and adopts a
pre-armed watchdog on probe so the chip is not briefly disarmed
during userspace ramp-up.

**Public release.** The driver makes its first user-facing appearance
in **v3.5.0**, shipping as `DRV_VERSION "1.0"` to match the convention
of the other rtl819x drivers (irq, gpio, timer, 8250, ...). The
internal "1.0 / 1.1 / 1.2" milestones referenced throughout this
document are *dev-cycle markers* — three iterations during the v3.5.0
prep window where successive WDT-### findings were closed — **not**
user-visible released versions.

Dev-cycle history, condensed:

* **1.0** — initial bring-up: WDTCNR write paths, restart handler,
  HW_RUNNING adoption, datasheet-correct OVSEL field.
* **1.1** — WDT-005 + WDT-007 closed: slowclk CDBR rework
  (see TMR-005 in `drivers/clocksource/AUDIT.md`), `S25watchdog`
  feeder activated, DT `timeout-sec=60`.
* **1.2** — WDT-008 + WDT-009 closed: soft-lockup blind spot plugged
  via a panic notifier + `BOOTPARAM_SOFTLOCKUP_PANIC=y`; notifier
  pinned to `INT_MAX` so a stuck higher-priority notifier cannot
  defeat the recovery write.

All three milestones are squashed into the **v3.5.0** release as
`DRV_VERSION "1.0"`.

The bit layout was initially reverse-engineered from the Realtek SDK
V3.4.7.3 BSP, then verified against the RTL8196E-CG datasheet
(Track ID JATR-3375-16, Rev. 1.0, table 27). Two SDK-era assumptions
turned out to be wrong:

* OVSEL is a **4-bit field** spanning bits `[22:21]` (low) and
  `[18:17]` (high), with ten valid encodings from `0000` (2^15
  ticks) to `1001` (2^24 ticks). The SDK only ever writes `0011`
  (its "longest" bucket), losing access to 64× more headroom.
* The kick bit at `[23]` is named **WDTCLR** in the datasheet —
  "Write 1 to clear the up-count watchdog counter" — and the
  watchdog itself is an *up-counter* that fires on overflow, not a
  countdown timer.

The chip also exposes a one-shot reset-cause indicator at WDIND
(bit 20), set by hardware on a watchdog-triggered reset and
write-1-to-clear.

## Summary of findings

9 findings total. 4 applied in `1.0`; WDT-005 / WDT-007 closed in
`1.1` (v3.4.2 — slowclk DT rework); WDT-008 + WDT-009 closed in `1.2`
(soft-lockup blind spot — panic notifier + `CONFIG_BOOTPARAM_SOFTLOCKUP_PANIC=y`,
panic-notifier priority pinned to `INT_MAX`). WDT-001 still partial
pending on-hardware reset-cause confirmation.

| ID | Type | Severity | Confidence | Status | One-liner |
|----|------|----------|------------|--------|-----------|
| WDT-001 | OBSERVABILITY | low | partial | **partially applied** | reset-cause decoded from WDIND; bit empirically reads 0 even after watchdog reset, needs investigation |
| WDT-002 | API / PLATFORM | medium | certain | **applied** | restart_handler at priority 192 supersedes arch_reset cleanly |
| WDT-003 | PLATFORM / API | medium | certain | **applied** | `of_match_table` restricted to `realtek,rtl8196e-wdt` |
| WDT-004 | ROBUSTNESS | medium | certain | **applied** | `WDOG_HW_RUNNING` adoption avoids briefly disarming a pre-armed chip |
| WDT-005 | PLATFORM / FUNCTIONAL | **HIGH** | certain | **applied (1.1)** | shared CDBR DivFactor moved 8→8000 via slowclk DT node; OVSEL=1001 overflow grows ~671 ms→~671 s |
| WDT-006 | API / DOCUMENTATION | low | certain | **applied** | OVSEL field corrected from 2-bit to 4-bit; driver now uses 1001 (2^24 ticks) instead of 0011 (2^18) |
| WDT-007 | OPERATIONAL | medium | certain | **applied (1.1)** | `S25watchdog` made executable + DT `timeout-sec` 1→60 — feeder now boots and kicks |
| WDT-008 | RECOVERY / FUNCTIONAL | **HIGH** | certain | **applied (1.2)** | soft-lockup blind spot: framework auto-kicker pets chip during userspace busy-syscall hangs; panic notifier + `BOOTPARAM_SOFTLOCKUP_PANIC=y` force reset in ~23 s |
| WDT-009 | RECOVERY / DEFENSE-IN-DEPTH | low | certain | **applied (1.2)** | panic notifier priority pinned to `INT_MAX` so a stuck higher-priority notifier cannot defeat the WDT-008 chip-arming write |

## Applied — driver 1.0

### WDT-002 — restart handler supersedes arch_reset

The arch-level `_machine_restart()` (in `arch/mips/realtek/setup.c`)
currently writes `0` to WDTCNR to trigger an immediate reset, and
that hook is installed at boot in `plat_mem_setup()`. The MIPS
reboot path (`do_kernel_restart()`) runs the registered
restart-notifier chain *before* falling through to
`_machine_restart`, so a watchdog driver that registers a
`watchdog_ops.restart` callback supersedes the arch path while it is
loaded.

`watchdog_set_restart_priority(&wdt->wdd, 192)` places this driver
above the default-priority handlers (128). The actual reset sequence
is identical to the arch path: `writel(0, base)` followed by an
`mdelay(50)` to let the chip pull the line.

**Net effect:** if the driver loaded successfully, `reboot` flows
through the registered handler. If the driver is unloaded, the
kernel falls back to `_machine_restart` from the arch code. Behaviour
visible to the operator is unchanged.

### WDT-003 — match-table tightening

The driver header documents the WDTCNR layout as RTL8196E-specific.
Other RTL819x variants (RTL8196C, RTL8197F) may have different
register positions or selector encodings. The `of_match_table` is
limited to a single `realtek,rtl8196e-wdt` entry; we will not bind
on `realtek,rtl819x-wdt` or other generic compatibles. Same
rationale as GPIO-003 in `drivers/gpio/AUDIT.md`.

### WDT-004 — `WDOG_HW_RUNNING` adoption

If the bootloader (or a previous Linux instance via `arch_reset`)
left the watchdog armed, WDTE in `[31:24]` is anything other than
`0xA5`. The driver detects this at probe and sets `WDOG_HW_RUNNING`
instead of issuing the disable pattern. The watchdog framework then
pings the chip every `wdd.timeout / 2` seconds (with
`CONFIG_WATCHDOG_HANDLE_BOOT_ENABLED=y`) until userspace opens
`/dev/watchdog`. Without this adoption, the brief window between
probe and the first userspace open would let the chip count up past
overflow and reset the box.

### WDT-006 — OVSEL is a 4-bit field, not 2-bit

The SDK V3.4.7.3 BSP (`boards/rtl8196e/bsp/timer.c`) writes only
`0x00600000` — bits `[22:21] = 11` — and never touches the upper
selector bits at `[18:17]`. Reading the SDK alone, OVSEL appeared to
be a 2-bit field giving four buckets (2^15 .. 2^18 ticks). The
datasheet is explicit: OVSEL is 4 bits, with ten valid encodings up
to `1001` (2^24 ticks).

Driver `1.0` uses OVSEL=`1001` so the gross hardware ceiling matches
what the chip is capable of.  Even with OVSEL=`1001` the practical
overflow is short — see WDT-005 — but at least the driver is no
longer leaving 6 of the 4 selector bits stranded.

### WDT-007 — feeder enabled in v3.4.2 (driver 1.1)

`34-Userdata/skeleton/etc/init.d/S25watchdog` mode now `0755`. With
WDT-005 closed, the BusyBox feeder kicks every 30 s against a ~671 s
hardware ceiling — comfortable. The DT `timeout-sec` was simultaneously
bumped 1 → 60 so the framework's heartbeat (when `WDOG_HW_RUNNING`
adoption fires) matches the kick cadence.

## Applied in 1.1 — v3.4.2

### WDT-005 — CDBR clock-sharing closed via slowclk DT node

The shared CDBR clock is now fed from a new 25 kHz `slowclk`
fixed-clock node in `rtl819x.dtsi`. `timer-rtl819x` programs
DivFactor = busclk/slowclk = 200 MHz / 25 kHz = 8000 (matching the
SDK BSP), so the watchdog tick drops from 25 MHz to 25 kHz. At
OVSEL=1001 the hardware overflow grows from ~671 ms to ~671 s; a
30 s BusyBox feeder now keeps the chip alive with ~22× margin.

Driver-side changes (v1.1):

* `WDT_TIMEOUT_SECS_DEFAULT` 1 → 60.
* `wdd.min_timeout`/`max_timeout` widened to `[1, 671]` (the chip's
  real range in seconds at 25 kHz CDBR).
* `set_timeout` actually honours the requested value rather than
  hard-clamping to 1 s. Hardware OVSEL stays at the max bucket
  unconditionally — `wdd.timeout` is the soft contract with
  userspace, not a register write.
* Probe banner reflects the new effective range.

DT-side changes:

* New `slowclk` fixed-clock @ 25 kHz in `rtl819x.dtsi`.
* `timer@3100` `clocks[0]` repointed `&refclk` → `&slowclk`.
* `watchdog@311c` `timeout-sec` bumped 1 → 60, `status = "disabled"`
  removed from the dtsi default (the board DTS already overrode it
  to "okay").

Clocksource-side changes — see TMR-005 in
`drivers/clocksource/AUDIT.md`. Key cross-impact: `clockevents`
`min_delta` reduced 0x300 → 8 because 0x300 ticks at 25 kHz = 30 ms,
which would have killed HZ=250 scheduling.

Trade-off: `sched_clock` granularity drops from 40 ns to 40 µs.
Acceptable for kernel timekeeping (HZ=250 = 4 ms tick), visible only
in perf/ftrace precision. Not a goal for this gateway workload.

## Deferred

### WDT-001 — WDIND empirically reads 0 after a watchdog reset

Datasheet description: "Watchdog Event Indicator. 0: A Watchdog
RESET did not occur (POWER-ON or PIN RESET). 1: A Watchdog RESET
occurred. Write '1' to clear."

Driver `1.0` decodes the bit and emits `dev_info("last reset:
watchdog timeout / power-on / pin reset", ...)` in dmesg, then
W1C-clears it. However on this rev. 0xb08 part, after a deliberate
watchdog-induced reset (OVSEL=1001 plus no kicks), the bit reads as
`0`. Two possible explanations:

* the bootloader (V2.6) clears WDIND somewhere in its early
  init — to verify by tracing the bootloader, or by reading
  WDTCNR over JTAG immediately after a watchdog event before the
  bootloader runs;
* the watchdog reset on this part is a full chip-level reset that
  also clears the WDTCNR register including WDIND — making the bit
  effectively useless for post-mortem on a reset cycle.

Either way, the cosmetic dmesg line is currently misleading on this
chip. Closing this finding requires either confirming the
bootloader-clears hypothesis (and patching the bootloader to leave
WDIND alone) or accepting that there is no surviving reset-cause
indicator on this SoC and removing the dmesg line.

## Validation

### v1.0 (v3.4.1) — on real hardware, RTL8196E rev. 0xb08, GW at 192.168.1.88

* Probe banner present at boot:
  `rtl819x-wdt 1800311c.watchdog: v1.0 registered, default timeout
  1s, nowayout=0`
* Bringup register dump (sysc+0x3100..0x3120) reads the timer block
  cleanly via `sr_r32()` (the DT syscon node only declares 0x1000
  bytes, so a regmap-based dump is rejected with -EIO).
* Direct `devmem` writes confirmed:
  * `0xA5000000` is the "stopped" default (matches WDTE=0xA5);
  * arming with OVSEL=0011 (SDK pattern) overflows in ~10 ms on
    this chip;
  * arming with OVSEL=1001 (`0x00A40000`) overflows around 300–600 ms;
  * WDTCLR auto-clears after the kick (post-write readback shows
    `0x00240000`, the OVSEL pattern alone).
* Userspace feeder kept the chip alive for less than 5 seconds
  with `watchdog -t 1` (kick interval > overflow window) — this is
  the live demonstration of WDT-005 and the reason the feeder is
  shipped non-executable.

### v1.1 (v3.4.2) — on real hardware, RTL8196E rev. 0xb08, GW at 192.168.1.88

* Probe banners present at boot:
  * `timer-rtl819x v1.0 (J. Nilo) - IRQ:7, CLK:25.000kHz, mult:107374, shift:32`
    — confirms slowclk DT node is read and CDBR DivFactor=8000.
  * `rtl819x-wdt 1800311c.watchdog: v1.1 registered, default timeout
    60s, nowayout=0`.
* Bringup register dump after a fresh boot reads `+0x3118 = 0x1f400000`
  (DivFactor = 0x1f40 = 8000, exactly the SDK BSP pattern) and live
  `WDTCNR = 0x00240000` after `start()` — decoding via the `WDT_OVSEL`
  macro confirms OVSEL=1001, the max-bucket arm pattern.
* `sched_clock: 28 bits at 25kHz, resolution 40000ns, wraps every
  5368709100000ns` (i.e. ~89 min half-wrap, vs. ~5.4 s at 25 MHz) —
  matches the granularity/overhead trade-off documented in
  `../clocksource/AUDIT.md` TMR-005.
* **HW overflow path validated end-to-end** via the `rtl819x_wdt_hangtest`
  procfs gadget (`CONFIG_RTL819X_WDT_HANGTEST`, debug-only, stripped
  after the measurement). A write to `/proc/wdt_hangtest` ran
  `local_irq_disable()` then `while (1) cpu_relax()`. The kernel
  framework kthread stopped pinging and the SoC reset itself; SSH was
  reachable again **666 s after the hang**, of which ~17 s is the
  bootloader+kernel boot, putting the HW reset at **~643 s ≈ 95.8 %
  of the OVSEL=1001 bucket nominal (671 s)**. The 28 s shortfall vs
  nominal lines up exactly with how far we were into the framework's
  30 s ping cycle when IRQs went off. End-to-end the proof points
  are: slowclk DT @ 25 kHz → driver picks OVSEL=1001 → without anyone
  pinging, the chip resets the SoC at the documented bucket time.
* `sysrq-b` (`echo b > /proc/sysrq-trigger`) reboots the box in ~1.3 s,
  exercising the `.restart` handler path (priority 192,
  `writel(0, base)` → OVSEL=0 = 2^15 ticks @ 25 kHz = 1.31 s).
* WDIND read-back quirk on rev 0xb08 persists from v1.0 — both the
  hangtest overflow and the `.restart`-triggered reset come back with
  `last reset: power-on / pin reset (WDTCNR=0xa5000000)` (WDIND=0).
  Tracked separately as WDT-001; not a regression.

#### iperf3 regression — `Gateway v3.4.2-instrumented`, 2026-05-11

Single-stream baselines hold within the ±1 Mbit/s measurement noise
and TCP retrans is essentially zero:

| Test | Measured | v3.4.1 baseline | Threshold |
|---|---|---|---|
| TCP RX (host → gw), 30 s | 93.1 Mbit/s, retrans 1/242886 = 0.0004 % | 93.7 | ≥ 92.9 ✓ |
| TCP TX (gw → host), 30 s | 70.1 Mbit/s, retrans 0/182899 | 70.0 | ≥ 69    ✓ |
| TCP stress, 300 s | 93.6 Mbit/s, retrans 19/2441326 = 0.0008 % | —   | stability ✓ |

Multi-stream parallel TCP (4× and 8×) sums to ~94 Mbit/s with
0.46–0.57 % retrans, which is normal stream-fairness behaviour
under chip saturation, not a regression. UDP 50/100 M loss
percentages reflect kernel `RcvbufErrors` (iperf3 not draining the
socket fast enough on a 200 MHz Lexra), not driver-side errors —
the `eth0` `errors:` counter is zero in both RX and TX.

Conclusion: slowclk CDBR change has no measurable cost on the TCP
fast path. `min_delta` rising 31 µs → 320 µs at the new tick rate
is well below the slowest legitimate hrtimer cadence on this
platform and does not interact with the NAPI / TX-kick coalescing
loop.

#### OTBR-RCP soak — delegated to beta-tester deployment

The lab does not normally pair end-devices to the dev gateway; the
authoritative long-running soak is `v3.4.2-instrumented` running in
@olivluca's production setup against issue #99. The ESP32 capture
handoff under `36-Debug-Capture/` is what makes that soak
post-mortem-able.

## How this maps to the public release

The dev-cycle 1.0 / 1.1 / 1.2 milestones tracked above are all
rolled into a single public release: **v3.5.0**, shipping as
`DRV_VERSION "1.0"` to match the convention of the other
rtl819x drivers (irq, gpio, timer, 8250, ...).

The `v3.4.2-instrumented` tag was a diagnostic build delivered
to beta tester olivluca for GitHub issue #99 — it carried the
dev-cycle 1.2 driver alongside the wait_tracer LD shim,
health-snap, and remote-syslog opt-in. None of that
instrumentation ships in v3.5.0; the driver work alone graduates
to the production release.

The intermediate `v3.4.1` and `v3.4.2` versions never carried
this driver — `v3.4.1` was tagged without it (the watchdog work
was reshuffled out for the "init scripts unchanged" promise of
the v3.4.x line), and `v3.4.2` was never released. v3.5.0 is
where it lands publicly for the first time.

## Applied — driver 1.2

### WDT-008 — soft-lockup blind spot plugged

**Symptom.** GitHub issue #99 (olivluca's beta-tester capture, 2026-05-09):
otbr-agent (PID 95) enters a busy-syscall loop in `__do_wait`. The
soft-lockup detector reports the hang at 22 s and keeps incrementing
the duration counter every 22 s. After 600+ seconds the box is still
spamming the soft-lockup banner, the hardware watchdog has *not*
fired, and recovery requires a manual power cycle. The whole point
of shipping the hardware watchdog in v3.4.2 was to reboot the box
autonomously on hangs of exactly this shape, so the silent failure
was a real gap.

**Root cause.** On UP + PREEMPT_NONE the watchdog-framework's
`WDOG_HW_RUNNING` auto-kicker hrtimer fires from softirq context,
which drains on every syscall return. A userspace busy-syscall loop
(fast-failing syscall, immediate retry) therefore lets the softirq
drain — and the auto-kicker — keep running on every iteration. The
kernel as a whole is *not* stuck (other tasks still run, IRQs are
served, the auto-kicker pets the chip every ~30 s), only the
offending *task* is stuck. The HW watchdog has no way to tell the
difference and stays armed-but-petted forever.

**Fix.** Two-line change, conceptually:

1. `CONFIG_BOOTPARAM_SOFTLOCKUP_PANIC=y` in `config-6.18-realtek.txt`
   — once the soft-lockup detector has confirmed the hang (22 s
   default), `kernel/watchdog.c:850` calls `panic("softlockup: hung
   tasks")`. The detector itself runs from a separate hrtimer that is
   not blocked by the busy-syscall loop, so it reliably fires.
2. Driver registers a `panic_notifier_list` callback at probe that
   writes `0` to `WDTCNR`. `panic()` calls `local_irq_disable()`
   before invoking the notifier chain, which halts the auto-kicker
   hrtimer on the only CPU. With auto-kicker silenced, our `0` write
   leaves the chip running with OVSEL=0 (smallest bucket ≈ 1.31 s at
   slowclk=25 kHz), WDTCLR=0 (not kicked). Reset fires within the
   bucket window.

The write pattern is identical to what `.restart` and the arch-level
`_machine_restart` already use; the notifier just bolts it onto the
panic path so a confirmed soft lockup goes the same way as a clean
`reboot`.

**Recovery latency.** ~22 s (soft-lockup threshold) + ~1.3 s (chip
overflow) ≈ 23 s end-to-end. Compared to "never" in the pre-fix
behaviour, this is the recovery-mechanism behaviour the v3.4.2
release notes implied.

**Defense in depth — why the notifier and not just the Kconfig.**
`CONFIG_PANIC_TIMEOUT=10` already makes `panic()` call
`emergency_restart()` after a 10 s busy delay, which on MIPS Realtek
ultimately writes `0` to WDTCNR via `_machine_restart`. So even
without the notifier, soft-lockup → panic → 10 s delay → emergency
restart would land the same write 11 s slower. The notifier wins
~10 s of recovery latency *and* survives the case where
`emergency_restart` itself wedges (e.g. wedged on a console flush in
the panic path). The chip overflow is hardware-driven the moment
IRQs are disabled.

**Affected files.**

* `drivers/watchdog/rtl819x_wdt.c` — panic notifier, struct field,
  devm cleanup hook. DRV_VERSION bumped to `1.2`.
* `config-6.18-realtek.txt` — flipped
  `CONFIG_BOOTPARAM_SOFTLOCKUP_PANIC` to `=y`.

**Validation.** Cold-boot to verify the panic notifier registers
without errors; trigger a panic via `echo c > /proc/sysrq-trigger`
(needs `CONFIG_MAGIC_SYSRQ=y`, present) and confirm the box reboots
within ~2 s of the panic banner rather than waiting on the 10 s
panic_timeout. End-to-end soft-lockup test is best done with the
wait_tracer-instrumented build on `v3.4.2-instrumented`: a real
otbr-agent hang on issue #99 reproducers exercises the full
detector → panic → notifier → chip-reset path.

### WDT-009 — panic notifier priority pinned to INT_MAX

**Symptom.** Surfaced by a fresh from-scratch audit of the 1.2 driver,
not by an observed failure: the panic notifier registered in WDT-008
never assigns `nb->priority`, which defaults to 0. The kernel panic
notifier chain dispatches notifiers in *descending* priority order,
so any notifier registered at a higher priority — or any future
distro/board patch that adds one — runs **before** the chip-arming
write. If such a prior-in-chain notifier wedged (console flush, flash
write, cross-call that never lands), our recovery write would never
execute, and the box would fall back to the `CONFIG_PANIC_TIMEOUT=10`
delay that WDT-008 was specifically designed to out-run.

No current chain pollution is observed on this kernel config — the
finding is forward-looking, defense-in-depth.

**Fix.** Two lines in `rtl819x_wdt_probe`:

```c
wdt->panic_nb.notifier_call = rtl819x_wdt_panic_notify;
wdt->panic_nb.priority      = INT_MAX;   /* run first, then continue */
ret = atomic_notifier_chain_register(&panic_notifier_list, &wdt->panic_nb);
```

`INT_MAX` is the canonical "head of the chain" sentinel; nothing else
in-tree registers above it. The notifier still returns `NOTIFY_DONE`,
so lower-priority notifiers continue to execute within the ~1.31 s
grace window between our chip-arm write and HW overflow — crashlog
dumpers, console flushers, etc. all still get their turn.

**Trade-off considered.** Running first means the chip reset is
*scheduled* before any crash-info dumpers run. We accept this because
(a) the chip does not reset instantly — OVSEL=0 gives 1.31 s of grace
time during which lower-priority notifiers in the same chain continue
to execute, and (b) the alternative — letting an arbitrary dumper
wedge the chain — was the failure mode WDT-008 itself was designed
to fix.

**Affected files.**

* `drivers/watchdog/rtl819x_wdt.c` — single-line probe addition, comment
  block in `rtl819x_wdt_panic_notify()` extended with the rationale.

**Validation.** Same as WDT-008 — cold-boot + `sysrq-c` reboots in
~2 s. The defense-in-depth aspect is not directly testable without
injecting a hostile higher-priority notifier; the meaningful check
is "no regression vs WDT-008 acceptance criterion".
