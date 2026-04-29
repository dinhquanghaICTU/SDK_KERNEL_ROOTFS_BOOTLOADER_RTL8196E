# Release v3.2.1 — April 29, 2026

A small patch on top of v3.2.0. Two related fixes around `/userdata/etc/radio.conf`, both reported by @skinkie in [#93](https://github.com/jnilo1/hacking-lidl-silvercrest-gateway/issues/93):

### `radio.conf` is now mandatory after install (#93)

A fresh install previously deleted `radio.conf` when you picked Zigbee mode — Zigbee vs. Thread was signalled purely by the file's *presence*. That left the in-kernel UART bridge with no reference for the EFR32 baud (driver default 460800), and any gateway whose chip is still at 115200 (Tuya stock, v2.x first flash) ended up mismatched out of the box.

`build_fullflash.sh` now always writes `radio.conf` with at minimum `FIRMWARE` and `FIRMWARE_BAUD`. The Zigbee default is `FIRMWARE=ncp` / `FIRMWARE_BAUD=115200` so a freshly-installed gateway talks to its EFR32 out of the box without needing `flash_efr32.sh`. The Thread default is `FIRMWARE=otrcp` / `FIRMWARE_BAUD=460800` / `MODE=otbr`. Either way, any later `flash_efr32.sh` rewrites these keys to match the actual chip state.

### `flash_efr32.sh` no longer claims success when radio.conf write fails (#93)

After the GBL upload, the script writes `FIRMWARE_BAUD` to the gateway's `radio.conf` over SSH so the bridge arms at the right speed on next boot. That call used to end with `2>/dev/null || true`, which silently masked any SSH failure — the script printed `Flash complete.` while the file on the gateway was stale or empty. @skinkie hit this in [#93](https://github.com/jnilo1/hacking-lidl-silvercrest-gateway/issues/93): chip flashed at the new baud, gateway-side bridge still on the old one, link broken after the reboot.

The write is now strictly checked. If it fails, you get an explicit error message with the exact `echo … > /userdata/etc/radio.conf` commands to fix it by hand, and a hint that re-running the script will pick up where it left off (the chip is already on the new firmware; the second run only updates `radio.conf`).

### Upgrade

```sh
./flash_install_rtl8196e.sh -y <gateway-IP>
```

In-place upgrade. Your existing `radio.conf` is preserved across the upgrade, so v3.2.0 → v3.2.1 introduces no migration friction. The new install-time defaults only apply to fresh installs.

---

Full technical changelog: [`3-Main-SoC-Realtek-RTL8196E/CHANGELOG.md`](3-Main-SoC-Realtek-RTL8196E/CHANGELOG.md#321---2026-04-29).
