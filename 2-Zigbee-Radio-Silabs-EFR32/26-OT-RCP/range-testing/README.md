# Thread Mesh Range Testing

Tools to characterise the radio link quality of a Thread mesh hosted by
the Lidl Silvercrest Gateway (custom firmware: `otbr-agent` +
EFR32MG1B OT-RCP). They let you measure, in your own deployment, how
**TX power**, **Thread channel**, **antenna orientation** and **sensor
orientation** affect the uplink RSSI, the LQI, and the attached child
count.

A field-test report covering all four phases, with practical
recommendations and recipes, is in [`REPORT.md`](REPORT.md).

## Layout

```
range-testing/
├── README.md             this file
├── REPORT.md             field-test results and recommendations
├── gateway/              scripts that run on the gateway (BusyBox sh)
│   ├── range_test.sh         core CSV sampler
│   ├── phase1_tx_sweep.sh    TX power sweep with abort-on-detach
│   ├── phase2_channel_migration.sh   channel migration via Pending Op Dataset
│   ├── orientation_runner.sh paced orientation runner (operator-confirmed)
│   ├── healthmon.sh          minute-resolution gateway health sampler (opt-in)
│   ├── ha_link_publisher.sh  Thread RSSI/LQI publisher to Home Assistant (opt-in)
│   ├── ha_link_publisher.conf.example   annotated config template
│   └── examples/             optional helpers (init scripts, …)
│       └── S75ha_link_publisher    auto-start init script for the publisher
└── analysis/             developer-machine tooling (Python 3.10+)
    ├── ha_matter_map.py  HA WS API → label/node_id/ext_mac mapping
    └── analyze.py        per-palier and per-sensor stats from CSVs
```

## Quick start

### 1. Install the gateway scripts

From your developer machine, copy the gateway-side scripts to the
gateway and make them available on `$PATH`:

```sh
scp gateway/range_test.sh root@192.168.1.88:/usr/bin/
scp gateway/phase1_tx_sweep.sh root@192.168.1.88:/usr/bin/
scp gateway/phase2_channel_migration.sh root@192.168.1.88:/usr/bin/
scp gateway/orientation_runner.sh root@192.168.1.88:/usr/bin/
scp gateway/healthmon.sh root@192.168.1.88:/usr/bin/   # optional

ssh root@192.168.1.88 'chmod +x /usr/bin/range_test.sh /usr/bin/phase1_tx_sweep.sh /usr/bin/phase2_channel_migration.sh /usr/bin/orientation_runner.sh /usr/bin/healthmon.sh'
```

`ot-ctl` must be on `$PATH` on the gateway. On the stock custom
firmware build it is at `/userdata/usr/bin/ot-ctl`; if it is not in
`$PATH`, set the `OT_CTL` environment variable when invoking the
scripts:

```sh
ssh root@192.168.1.88 'OT_CTL=/userdata/usr/bin/ot-ctl /usr/bin/range_test.sh smoke 60 10'
```

### 2. Run a single sample (sanity check)

```sh
ssh root@192.168.1.88 'OT_CTL=/userdata/usr/bin/ot-ctl /usr/bin/range_test.sh smoke 120 30'
```

This polls the neighbour table every 30 s for 2 minutes and writes
`/userdata/log/range_smoke.csv`. Pull it back with `scp` for analysis.

### 3. Set up the Python analysis venv (developer machine)

```sh
python3 -m venv .venv
.venv/bin/pip install websockets    # only ha_matter_map.py needs it
```

`analyze.py` is pure-stdlib and does not need the venv.

### 4. Build the HA label map (optional but recommended)

If you use Matter sensors, their Thread `ext_mac` is randomised at every
commissioning, so the CSVs cannot be read by themselves. Build a map
from your Home Assistant once:

```sh
HA_URL=homeassistant.local:8123 HA_TOKEN=<bearer> \
  .venv/bin/python analysis/ha_matter_map.py > labels.csv
```

`<bearer>` is a long-lived access token created from
HA → Profile → Security → Long-Lived Access Tokens.

### 5. Compare paliers

```sh
analysis/analyze.py --map labels.csv \
                    --ext_mac <hex> \
                    range_phase4_*.csv
```

`--ext_mac` is optional; without it, `analyze.py` reports stats for
every sensor seen in the input CSVs.

## Test phases at a glance

| Phase | Script | What it varies | Operator action needed |
|:--|:--|:--|:--|
| 1 — TX power | `phase1_tx_sweep.sh` | TX power, in calibrated steps | none (fully scripted) |
| 2 — Channel  | `phase2_channel_migration.sh` | 802.15.4 channel | none |
| 3 — Gateway orientation | `orientation_runner.sh subject=gateway` | physical pose of gateway | rotate gateway between paliers |
| 4 — Sensor orientation  | `orientation_runner.sh subject=<sensor>` | physical pose of one sensor | rotate that sensor between paliers |

The orientation runner waits for an operator ack between paliers
(`touch /tmp/orientation_ack`). Phase 1 / 2 are fully autonomous, but
the user should monitor the run since `phase1` aborts on detach and
`phase2` aborts if a migration fails.

### Optional: surface link quality in Home Assistant

`ha_link_publisher.sh` exposes the gateway-side **uplink RSSI**, **LQI**
and **last-seen age** of every Thread child as Home Assistant entities,
so mesh degradations are visible in the same dashboards as the sensors'
actual measurements. One HA entity per device, multi-attribute payload:

```yaml
sensor.thread_<slug>_rssi:
  state: -73                  # avg RSSI in dBm
  attributes:
    unit_of_measurement: dBm
    device_class: signal_strength
    friendly_name: "MYGGBETT 5202 RSSI"
    lqi: 3
    last_rssi: -73
    age_s: 7
    rloc: "0xb417"
    ext_mac: "563784e9bea2ed1b"
    attached: true
```

Resource footprint at the default 60 s cadence with 16 sensors: ~1 % of
one CPU core, ~500 KB transient RSS, ~24 MB/day of HA traffic, **zero
JFFS2 wear**. Lighter than `healthmon.sh`. See `REPORT.md` resource
budget for the breakdown.

**Setup**:

1. **Create a long-lived access token** in HA → Profile → Security →
   Long-Lived Access Tokens → Create Token. Read+write on `/api/states/`
   is sufficient.
2. **Generate a label map** from your existing Matter pairings:
   ```sh
   HA_URL=homeassistant.local:8123 HA_TOKEN=<bearer> \
     .venv/bin/python analysis/ha_matter_map.py | \
     awk -F, 'NR>1 {printf "LABEL_%s=\"%s\"\n", $3, $1}'
   ```
3. **Build the conf** by copying `ha_link_publisher.conf.example`,
   filling in `HA_URL` + `HA_TOKEN`, and pasting the labels from step 2:
   ```sh
   cp gateway/ha_link_publisher.conf.example ha_link_publisher.conf
   # …edit ha_link_publisher.conf…
   scp -O ha_link_publisher.conf root@192.168.1.88:/userdata/etc/
   ```
4. **Deploy the script** and smoke-test:
   ```sh
   scp -O gateway/ha_link_publisher.sh root@192.168.1.88:/usr/bin/
   ssh root@192.168.1.88 'chmod +x /usr/bin/ha_link_publisher.sh && \
                          OT_CTL=/userdata/usr/bin/ot-ctl \
                          /usr/bin/ha_link_publisher.sh once'
   ```
   You should see one `posted sensor.thread_…_rssi …` line per attached
   child. Open HA → Developer Tools → States and search `sensor.thread_`
   to verify the entities appeared.
5. **Run as a daemon**:
   ```sh
   ssh root@192.168.1.88 '/usr/bin/ha_link_publisher.sh start'
   ssh root@192.168.1.88 '/usr/bin/ha_link_publisher.sh status'
   # to stop:
   ssh root@192.168.1.88 '/usr/bin/ha_link_publisher.sh stop'
   ```

6. **(Optional) auto-start at boot** — install the example init script:
   ```sh
   scp -O gateway/examples/S75ha_link_publisher root@192.168.1.88:/userdata/etc/init.d/
   ssh root@192.168.1.88 'chmod +x /userdata/etc/init.d/S75ha_link_publisher'
   ```
   The script is gated on the conf file being present, so it stays
   inert if the publisher is later disabled by removing
   `/userdata/etc/ha_link_publisher.conf`. It is **not** part of the
   default rootfs skeleton — installation is per-gateway.

**Detached devices**: when a sensor leaves the neighbor table, the
publisher keeps its HA entity alive with `attached: false` and a growing
`age_s`. Build a "stale" badge from `age_s > 240` if you want a one-look
mesh-health card.

### Optional: capture host-side context with `healthmon.sh`

For long runs (multi-hour soak, channel migration, orientation tests),
launch `healthmon.sh start` on the gateway in parallel. It samples
memory, CPU load, UART1 error counters (the OT-RCP link), Thread role
and child count, and Ethernet errors at 1 Hz/min. Useful for spotting
host-side anomalies — UART bit-errors, leaks, detach storms — that
would otherwise be invisible from the RSSI/LQI CSV alone.

```sh
ssh root@192.168.1.88 'OT_CTL=/userdata/usr/bin/ot-ctl /usr/bin/healthmon.sh start'
# ... run your range test ...
ssh root@192.168.1.88 '/usr/bin/healthmon.sh stop'
scp -O root@192.168.1.88:/userdata/log/health.csv .
```

`healthmon.sh status` prints the running PID and current log sizes.
On stop, it dumps the kernel ring buffer to `dmesg.snapshot` (without
clearing the buffer) so any anomaly visible in `health.csv` can be
correlated with kernel-level events.

## Caveats

- **Uplink-only**: all RSSI/LQI values are measured at the gateway from
  incoming child frames. The downlink (gateway → sensor) is not
  directly observable; you only see its quality through detach events.
- **N=1 deployment**: the recommendations in `REPORT.md` come from a
  single 16-sensor home. Repeat the tests in your own environment for
  defensible numbers — this is exactly what these scripts are for.
- **Matter ext_mac rotation**: rebuild `labels.csv` whenever you re-pair
  a Matter device, because its ext_mac will change.
