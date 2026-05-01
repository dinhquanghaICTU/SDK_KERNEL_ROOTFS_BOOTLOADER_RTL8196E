# RTL8196E GPIO driver — robustness / API audit

Target: Linux 6.18 port of the `gpio-rtl819x` GPIO bank driver for the
Realtek RTL8196E SoC. Exposes 32 GPIOs (4 ports × 8 bits) via gpiolib,
with PIN_MUX_SEL_2 (syscon offset `0x44`) pinmux for B2-B6 shared with
the LED ports.

Audit date: 2026-05-01. Driver version at audit time: pre-`1.0`
(unversioned) → bumped to `1.0` as part of this pass.

The driver is short (~300 lines) and already carefully scoped:
single-port MMIO + syscon regmap, no DMA, no buffers, no IRQ chip on
this bank (gpiolib `to_irq` not implemented — out of scope per the
audit). Audit focus was on the gpiolib lifecycle alignment with modern
6.x conventions, error propagation through the pinmux path, and DT
match-table tightness.

## Summary of findings

6 findings total. 3 fixed in driver `1.0`, 3 deferred (intentional
limitations or hardware-test-required).

| ID | Type | Severity | Confidence | Status | One-liner |
|----|------|----------|------------|--------|-----------|
| GPIO-001 | API / ROBUSTNESS | medium | certain | **fixed** | `gc.base = 0` (deprecated legacy global numbering) |
| GPIO-002 | ROBUSTNESS / PLATFORM | medium | probable | **fixed** | `regmap_update_bits` return ignored in pinmux path |
| GPIO-003 | PLATFORM / API | medium | certain | **fixed** | `of_match_table` accepted overly-broad compatibles |
| GPIO-004 | ROBUSTNESS / API | low | certain | **deferred** | no IRQ chip exposed despite ISR/IMR registers — needs HW spec |
| GPIO-005 | PLATFORM / ROBUSTNESS | low | hypothesis | **deferred** | shared B2 hardwired to switch ASIC LED — board-specific concern |
| GPIO-006 | ROBUSTNESS / PLATFORM | low | probable | **deferred** | `free()` does not restore pinmux — needs policy decision |

## Applied fixes — driver 1.0

All commits on `private/main` between `d815c54..4c7d029`. Detailed
mapping:

### GPIO-001 — dynamic GPIO base (-1) instead of hardcoded 0

Commit `d815c54`. The driver pinned `gc.base = 0`, which is the legacy
global GPIO numbering pattern that struct gpio_chip in 6.x explicitly
deprecates (non-negative bases). All in-tree DT consumers go through
phandles (`<&gpio0 N ...>`), so a fixed base provides no benefit and
risks collisions if a second gpiochip is ever added downstream.

Switch to `gc.base = -1` to let gpiolib allocate the chip range
dynamically. Verified: `rtl8196e.dts` (the only board using this
driver) references the status LED via `<&gpio0 11 ...>` and never as
a global GPIO number.

### GPIO-002 — propagate regmap_update_bits error from pinmux setup

Commit `73caf82`. `rtl819x_gpio_configure_pinmux()` returned `void`
and the `regmap_update_bits()` return was discarded. A syscon write
failure would silently leave a B2-B6 line with its LED_PORT mux still
in peripheral mode while the GPIO request reported success — gpiolib
would then hand out a line whose physical pin still drives the
shared LED function.

Plumb a real return code through `configure_pinmux()` and surface it
from `.request()`, with a `dev_err()` so the failure shows up in
dmesg. Also pulled the pinmux call out from under `rg->lock`:
regmap-syscon on RTL8196E is `fast_io` (MMIO-backed, no sleep) so
nesting the locks bought nothing, and the GPIO MMIO RMW below already
has its own protection.

### GPIO-003 — match only realtek,rtl8196e-gpio compatible

Commit `a465e14`. The driver header explicitly states that the
PIN_MUX_SEL_2 layout it writes (offset `0x44`, B2-B6 fields) is
RTL8196E-specific and may differ on RTL8196C / RTL8197F. Yet the
`of_match_table` accepted three strings, including the generic
`realtek,realtek-gpio` and `realtek,rtl819x-gpio`. A future DTS
targeting another RTL819x variant could therefore bind this driver
and corrupt syscon bits that mean something completely different
(UART / Ethernet / LED mux).

Tighten the match table to the exact `realtek,rtl8196e-gpio` string
and update the `rtl819x.dtsi` base node accordingly. The board file
`rtl8196e.dts` only references `gpio0` by phandle, so no consumer
breaks.

### Version bump 1.0 + probe banner

Commit `4c7d029`. Added `DRV_VERSION "1.0"`, exposed via
`MODULE_VERSION`, and tagged the probe `dev_info()` with it.

## Deferred — intentional or HW-test-required

### GPIO-004 — no IRQ chip exposed

The bank has `ISR` (offset `0x10`) and `IMR` (offset `0x14`) registers
documented in the header but not wired through gpiolib. No `to_irq`,
no `gpio_irq_chip`, no handler. Consumers that want to use a GPIO as
an interrupt source (e.g. a future button on a physical line other
than the front-panel one — which uses GPIO 9 polled from userspace
via `s40button`) cannot.

Not implemented in 1.0 because the audit's own recommendation was
"no patch without HW characterisation" — polarity, edge-vs-level,
clear semantics, parent IRQ all need to be confirmed against the
RTL8196E datasheet (which we do not have authoritatively). An
approximate irqchip risks interrupt storm or lost edges, much worse
than the current "no IRQ" state. Revisit when the use-case materialises.

### GPIO-005 — `valid_mask` for shared / hardwired pins

GPIO 10 (B2) is documented in the DTSI as hardwired to the switch
ASIC LED output and has no physical effect when driven as a regular
GPIO. Other B2-B6 pins are shared with LED_PORT0..4. The audit
suggested `gpio-reserved-ranges` or `init_valid_mask` to filter them
out.

Not applied in 1.0 because masking is board-specific and affects
operator workflows on adjacent boards using the same SoC. The current
behaviour (any GPIO request succeeds, pinmux is configured if needed)
is permissive but not unsafe. Revisit if a concrete misuse is observed.

### GPIO-006 — `free()` does not restore pinmux

Once a B2-B6 line is requested in GPIO mode, it stays in GPIO mode
even after gpiolib's `free()`. The audit suggested capturing the
initial state at `request()` and restoring on `free()`, but a
simplistic rollback could break a permanent consumer. Needs a policy
decision (per-board?) before being implemented.

## Validation

* Probe banner `gpio-rtl819x ... v1.0 (J. Nilo) - registered 32 GPIOs`.
* `/sys/class/gpio/` shows `chip0` with the dynamic base allocation
  (no longer pinned at 0).
* LED status (GPIO 11 via `leds-gpio-pwm`) toggles as expected through
  `/sys/class/leds/status/brightness`.
* Probe rebind / module unload test not run (driver is built-in via
  `module_platform_driver` but `CONFIG_GPIO_RTL819X=y`); devm cleanup
  is correct by inspection.

## How this maps to the public release

Driver `1.0` ships in **v3.4.0**.
