# Release v3.4.0 — May 1, 2026

Hardening release. Four independent driver audits applied as bounded patch sets across the custom RTL8196E kernel drivers (timer, IRQ controller, GPIO bank, Ethernet), one user-visible perf tuning of the IRQ routing on the Zigbee path, and the front-panel button daemon rewrite that fixes the v3.2.x / v3.3.0 intermittent SIGSEGV. No EFR32 firmware change, no breaking change to `radio.conf` or sysfs interfaces.

Validated end-to-end on real hardware: TCP RX 93.9 / TX 70.2 Mbit/s vs 93.9 / 71 baseline, 5-min stress retransmit rate 0.00 %, OTBR-RCP 460800 baud overnight soak 8h+ stable with two paired Sleepy End Devices.

---

### `timer-rtl819x` — clocksource / clockevent driver, now v1.0

Four findings from the timer audit applied:

* **`request_irq()` now happens before `clockevents_config_and_register()`**, with Timer0 explicitly disabled and any stale `TC0_PENDING` cleared in between. The previous order let the clockevent core potentially drive `set_next_event()` before the IRQ handler was installed; on this platform CPU IP7 is level-triggered and dedicated to Timer0, so an unhandled assertion would turn into an interrupt storm rather than a single lost edge.
* **Clock divider validated.** `clk_prepare_enable(busclk)` return is checked, `bus_rate >= timer_rate` is enforced, computed `div_fac` is bounded to the 16-bit register field. In practice the DT pins busclk=200 MHz / refclk=25 MHz so `div_fac=8` always, but a misconfigured DT used to silently program a bogus divider.
* **`clocksource_register_hz()` errors are now propagated.** No more silently registering `sched_clock` against a clocksource the kernel rejected.
* **The IRQ handler returns `IRQ_NONE` when `TC0_PENDING` is not set.** Lets the kernel spurious-IRQ machinery catch a misrouted IP7.

### `irq-rtl819x` — INTC driver, now v1.0

Three audit findings plus one perf tuning for this gateway's actual workload.

* **GIMR starts with only TC0 unmasked.** Other child sources (UART0/UART1/Switch) are activated through their `.irq_unmask` callback when the consumer driver calls `request_irq()` / `enable_irq()`, the way modern irqchips work. TC0 stays unconditional because the timer driver requests CPU IRQ 7 directly — the only hardware path TC0→IP7 is via INTC IRR1 + GIMR, verified against the open-source bootloader.
* **DT now describes the three parent IRQs explicitly** (IP2 cascade + IP3 + IP4). The driver resolves them with `irq_of_parse_and_map()` instead of hardcoding `REALTEK_CPU_IRQ_*` constants — removes an implicit dependency on cpuintc legacy domain numbering.
* **One MMIO write per IRQ saved** in the chained handler: GISR was being W1C'd both by the parent and by `realtek_soc_irq_ack()` via the level flow handler.
* **UART1 and Switch swapped on IP4/IP3.** `plat_irq_dispatch()` services IP lines in order IP7 > IP4 > IP3 > IP2. For this gateway, UART1 carries the Zigbee link to the EFR32 — 16-byte RX FIFO, ~350 µs of latency budget at 460800 baud, an overrun loses a Zigbee frame and forces Z2M / ZHA to reconnect (user-visible). The Ethernet switch uses DMA rings + NAPI, so a delayed switch IRQ at most translates to a TCP retransmit (invisible). Asymmetric benefit for the actual workload. Validated by the overnight OTBR 460800 soak: zero overruns on `ttyS1` over 8h+.

### `gpio-rtl819x` — GPIO bank driver, now v1.0

Three small findings:

* **Dynamic GPIO base (`-1`)** instead of hardcoded `0`. All in-tree DT consumers go through phandles so no consumer breaks; aligns with the modern gpiolib convention.
* **`regmap_update_bits` errors are now propagated.** A syscon write failure surfaces as `dev_err` + `.request` failure, instead of gpiolib silently handing out a line whose physical pin is still driving the shared LED function.
* **Match table tightened to `realtek,rtl8196e-gpio` only.** The driver header explicitly states the PIN_MUX_SEL_2 layout is RTL8196E-specific; the previous generic `realtek,realtek-gpio` and `realtek,rtl819x-gpio` entries are removed and the DT is updated accordingly. A future DT for another RTL819x variant won't bind this driver and corrupt syscon bits that mean something completely different.

### `rtl8196e-eth` — Ethernet driver, v2.3 → v2.4 (audit pass-2)

Second independent audit on the rewritten Ethernet driver. Four patches applied:

* **Short TX frames are now zero-padded before DMA.** The previous code flushed `max(skb->len, ETH_ZLEN)` bytes from `skb->data`, but never extended the SKB itself — the bytes between `skb->len` and 60 came from slab tailroom, and the switch DMA then transmitted them on the wire. Calling `skb_put_padto(skb, ETH_ZLEN)` extends the SKB and zero-fills the new tail. Closes a low-impact information leak on short frames (ARP, IPv4 minimal).
* **Both rings reset on `.ndo_stop`.** `ip link set eth0 down/up` under live traffic now starts each cycle with TX and RX rings rebuilt from the shadow SKB pool. The previous stop() left descriptors in indeterminate ownership, and the next open() only reprogrammed the ring base addresses. Validated with a 50-cycle down/up loop: zero `rx_errors`, zero `tx_errors`.
* **Descriptor layout pinned at compile time.** The hardware writes `ph_len` / `ph_flags` / `ph_reason` and reads `m_data` / `m_extbuf` at fixed byte offsets in shared memory — the GCC layout of `struct rtl_pktHdr` / `rtl_mBuf` is part of the silicon ABI. A `BUILD_BUG_ON` block fails the build on any silent shift, instead of corrupting RX/TX at runtime on the next struct edit or toolchain bump.
* **DT port masks validated against the 9-port hardware.** `member-ports` must fit in `0x1ff`; `untag-ports` must be a subset of `member-ports`. The HW iterates over ports 0..8 so out-of-range bits previously cycled through table writes on imaginary ports without error.

The local `AUDIT.md` is extended with a "Second-pass audit" section cross-referencing the new finding IDs against the existing 17 from the April pass — in particular it records that ETH-005 is the same item as the previously-documented intentional KSEG1 design (F17), and ETH-008 is the same proposal as F13 which was already tested on hardware in April and rejected (-47 Mb/s RX regression in the F11+F13+F15 bundle). Re-litigation prevention.

### Userdata — `s40button` rewrite

The v3.2.x / v3.3.0 BusyBox shell loop polling GPIO 9 via `devmem` had an intermittent SIGSEGV in the `ash` interpreter after some hours of idle polling. Rewritten as a static C daemon, ~112 KB, Lexra musl toolchain. Same observable behaviour (100 ms poll, 3-sample debounce, 5 s long-press → `recover_efr32 -q`, status LED visual feedback during the hold), no more interpreter crash. Built by `build_s40button.sh` and installed into `skeleton/usr/sbin/`.

### Validation summary

* `BUILD_BUG_ON` descriptor layout asserts hold on the Lexra MIPS toolchain.
* Boot-time banners present for all four updated drivers.
* iperf full suite: TCP RX 93.9 Mbit/s (= baseline), TCP TX 70.2 Mbit/s (vs baseline 71), TCP 5-min stress 93.9 Mbit/s with retransmit rate 0.00 % (15 of 2.44M segments), TCP parallel 4/8 streams 95.2 / 95.6 Mbit/s, UDP 50 M loss 0.029 %.
* `ip link set eth0 down/up` × 50 under no traffic: zero `rx_errors`, zero `tx_errors`, ping6 OK after.
* Five back-to-back `reboot` cycles: clean every time, all four banners stable, zero new warnings.
* Overnight OTBR-RCP soak at 460800 baud, two paired Sleepy End Devices: 8h38 with `otbr-agent` PID stable, zero `HandleRcpTimeout` / `RESET_UNKNOWN`, ttyS1 LSR shows `THRE | TEMT` only (no overrun bit).

### Upgrade

```sh
./flash_install_rtl8196e.sh -y <gateway-IP>
```

No `radio.conf` migration. sysfs interface unchanged.

### EFR32 firmwares

Unchanged from v3.3.0 — same `.gbl` artefacts, same NCP / RCP / OT-RCP / Router builds. Re-flashing the radio is not required for this release.

### Audit trail

Each kernel driver now carries its own `AUDIT.md` next to the source:

* `files-6.18/drivers/clocksource/AUDIT.md` — timer (TMR-001..004)
* `files-6.18/drivers/irqchip/AUDIT.md` — INTC (IRQ-001..007 + perf swap)
* `files-6.18/drivers/gpio/AUDIT.md` — GPIO bank (GPIO-001..006)
* `files-6.18/drivers/net/ethernet/rtl8196e-eth/AUDIT.md` — Ethernet (F1..F17 from April + ETH-001..008 from May pass-2)

Each file maps every finding ID to its commit SHA, status (fixed / deferred / rejected) and reasoning — including the rejected ones, so a future audit pass does not re-litigate decisions already made on hardware. Convention going forward: an audit pass landing as a coherent commit batch gets a `## Post-audit pass N (date) — driver M.N` section appended to the local `AUDIT.md`.
