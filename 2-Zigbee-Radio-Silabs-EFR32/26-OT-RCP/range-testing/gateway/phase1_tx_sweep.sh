#!/bin/sh
# phase1_tx_sweep.sh — TX power sweep with abort-on-detach safety
#
# Steps the OT-RCP transmit power down through a list of paliers, runs a
# `range_test.sh` sample at each step, and aborts (restoring TX to a safe
# value) if any child detaches. Use this to find the minimum TX power that
# keeps your full mesh attached, in your own deployment.
#
# Why it matters:
#   The EFR32MG1B radio's calibrated TX steps are 0, 1, 3, 5, 7, 10 dBm.
#   Any other request rounds to the nearest calibrated step. Lower TX
#   reduces 2.4 GHz pollution (helpful when WiFi shares the band) and
#   slightly extends battery on routers, at the cost of margin on the
#   weakest links. The sweep finds your local trade-off.
#
# Usage:
#   phase1_tx_sweep.sh [paliers="10:tx10 7:tx07 5:tx05 3:tx03 1:tx01 0:tx00"]
#                     [expected_children=N]
#                     [stab_sec=120]
#                     [sample_sec=600]
#                     [restore_tx=7]
#
# All arguments accepted as `key=value`, in any order. Example:
#   phase1_tx_sweep.sh expected_children=16 stab_sec=120 sample_sec=300 \
#                      paliers="7:tx07 5:tx05 3:tx03 1:tx01"
#
# Output:
#   $LOG_DIR/range_<label>.csv per palier (default LOG_DIR=/userdata/log)
#   /tmp/phase1_tx_sweep.log   summary log
#
# Environment variables:
#   OT_CTL  path to ot-ctl (default: ot-ctl on $PATH)
#   RT      path to range_test.sh (default: range_test.sh on $PATH)
#
# Behaviour:
# - At each palier, request the TX value, wait stab_sec for stabilisation,
#   then call range_test.sh for sample_sec. After the sample, count
#   attached children via `ot-ctl child table`. If fewer than
#   expected_children, restore TX to restore_tx and exit non-zero.
# - On SIGINT/SIGTERM, restore TX to restore_tx before exiting.
# - The TX value reported by ot-ctl after `ot-ctl txpower N` may differ
#   from N (rounded to the nearest calibrated step). The actual value is
#   logged into the per-palier CSV header.

set -u

# Defaults
PALIERS="10:tx10 7:tx07 5:tx05 3:tx03 1:tx01 0:tx00"
EXPECTED_CHILDREN=""
STAB_SEC=120
SAMPLE_SEC=600
RESTORE_TX=7

# Parse key=value args
for arg in "$@"; do
    case "$arg" in
        paliers=*)            PALIERS="${arg#paliers=}" ;;
        expected_children=*)  EXPECTED_CHILDREN="${arg#expected_children=}" ;;
        stab_sec=*)           STAB_SEC="${arg#stab_sec=}" ;;
        sample_sec=*)         SAMPLE_SEC="${arg#sample_sec=}" ;;
        restore_tx=*)         RESTORE_TX="${arg#restore_tx=}" ;;
        -h|--help)
            grep '^#' "$0" | head -40 | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *)
            echo "unknown arg: $arg" >&2
            exit 2 ;;
    esac
done

OT_CTL="${OT_CTL:-ot-ctl}"
RT="${RT:-range_test.sh}"
SUMMARY=/tmp/phase1_tx_sweep.log
: > "$SUMMARY"

log() {
    echo "[$(date)] $*" | tee -a "$SUMMARY"
}

children_count() {
    "$OT_CTL" child table 2>/dev/null | sed 's/\r$//' | \
        awk -F'|' '/^\| *[0-9]/ {n++} END {print 0+n}'
}

# Auto-detect expected_children from current state if not set
if [ -z "$EXPECTED_CHILDREN" ]; then
    EXPECTED_CHILDREN=$(children_count)
    log "expected_children auto-detected: $EXPECTED_CHILDREN"
fi

restore_tx() {
    "$OT_CTL" txpower "$RESTORE_TX" >/dev/null 2>&1
    log "TX restored to ${RESTORE_TX} dBm"
}

trap 'restore_tx; exit 130' INT TERM

log "=== TX sweep starting ==="
log "paliers=$PALIERS expected_children=$EXPECTED_CHILDREN"
log "stab_sec=$STAB_SEC sample_sec=$SAMPLE_SEC restore_tx=$RESTORE_TX"

for p in $PALIERS; do
    req=${p%%:*}
    label=${p##*:}

    log "--- $label: requesting TX=$req dBm ---"
    "$OT_CTL" txpower "$req" >/dev/null 2>&1
    sleep "$STAB_SEC" & wait $!

    actual=$("$OT_CTL" txpower 2>/dev/null | sed 's/\r$//' | \
              grep -oE '[0-9-]+ dBm' | head -1)
    log "$label: actual TX=$actual; sampling ${SAMPLE_SEC}s"

    "$RT" "$label" "$SAMPLE_SEC" 30 </dev/null >>/tmp/range.log 2>&1

    n=$(children_count)
    log "$label: children attached = $n / $EXPECTED_CHILDREN"

    if [ "$n" -lt "$EXPECTED_CHILDREN" ]; then
        log "ABORT at $label: child count $n < $EXPECTED_CHILDREN"
        restore_tx
        exit 1
    fi
done

log "=== TX sweep complete (all paliers OK) ==="
restore_tx
exit 0
