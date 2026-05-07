#!/bin/sh
# phase2_channel_migration.sh — Thread channel migration test
#
# Sample the current channel, migrate to a target channel via the Pending
# Operational Dataset, sample again, then migrate back. Lets users compare
# whether a different 802.15.4 channel improves their mesh in the presence
# of WiFi/microwave/etc. interference.
#
# Why a Pending Operational Dataset, not a "set channel" command:
#   The Pending Operational Dataset is the standard Thread mechanism for
#   coordinated channel migration. It schedules the change with a
#   future activation time and a propagation delay so that all routers and
#   children switch in lockstep — children that are asleep through the
#   announcement still wake up on the new channel because the dataset is
#   relayed at the next data poll. A direct `channel <N>` would only move
#   the leader and orphan everyone else.
#
# Usage:
#   phase2_channel_migration.sh [from=<chan>] [to=<chan>] \
#                               [delay_ms=120000] [settle_sec=180] \
#                               [sample_sec=600]
#
# Defaults: from=current, to=26 (or 15 if current=26), delay_ms=120000,
#           settle_sec=180, sample_sec=600
#
# All args use key=value form. Example, ch15 → ch20 → ch15 with shorter
# samples for a quick test:
#   phase2_channel_migration.sh from=15 to=20 sample_sec=300
#
# Output:
#   $LOG_DIR/range_phase2_ch<from>a.csv  baseline on starting channel
#   $LOG_DIR/range_phase2_ch<to>.csv     after migration
#   $LOG_DIR/range_phase2_ch<from>b.csv  back-migration control sample
#   /tmp/phase2_channel_migration.log    summary log
#
# Environment variables:
#   OT_CTL  path to ot-ctl     (default: ot-ctl on $PATH)
#   RT      path to range_test (default: range_test.sh on $PATH)
#
# Behaviour:
# - At each migration, the script writes a Pending Operational Dataset
#   with the desired channel, a `delay` of <delay_ms> milliseconds, and
#   timestamps that are guaranteed to be greater than the active dataset's
#   timestamps (otherwise the network would reject the pending dataset).
# - After committing the pending dataset, the script waits <settle_sec>
#   for the switchover and stabilisation, then verifies the channel and
#   the child count. If the channel did not change, the script aborts —
#   leaving the network on whatever channel it ended up on.
# - The starting channel is always treated as the "home" channel; the
#   script always migrates back to it at the end so the test is
#   non-destructive.

set -u

# Defaults
FROM=""
TO=""
DELAY_MS=120000
SETTLE_SEC=180
SAMPLE_SEC=600

for arg in "$@"; do
    case "$arg" in
        from=*)        FROM="${arg#from=}" ;;
        to=*)          TO="${arg#to=}" ;;
        delay_ms=*)    DELAY_MS="${arg#delay_ms=}" ;;
        settle_sec=*)  SETTLE_SEC="${arg#settle_sec=}" ;;
        sample_sec=*)  SAMPLE_SEC="${arg#sample_sec=}" ;;
        -h|--help)
            grep '^#' "$0" | head -45 | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *)
            echo "unknown arg: $arg" >&2
            exit 2 ;;
    esac
done

OT_CTL="${OT_CTL:-ot-ctl}"
RT="${RT:-range_test.sh}"
SUMMARY=/tmp/phase2_channel_migration.log
: > "$SUMMARY"

log() {
    echo "[$(date)] $*" | tee -a "$SUMMARY"
}

current_channel() {
    "$OT_CTL" channel 2>/dev/null | sed 's/\r$//' | grep -E '^[0-9]+$' | head -1
}

children_count() {
    "$OT_CTL" child table 2>/dev/null | sed 's/\r$//' | \
        awk -F'|' '/^\| *[0-9]/ {n++} END {print 0+n}'
}

migrate_to() {
    target=$1
    log "migration: starting Pending Operational Dataset for ch$target (delay ${DELAY_MS}ms)"
    "$OT_CTL" dataset init active     >/dev/null 2>&1
    "$OT_CTL" dataset channel "$target" >/dev/null 2>&1
    now_us=$(date +%s)
    "$OT_CTL" dataset pendingtimestamp "${now_us}000000"             >/dev/null 2>&1
    "$OT_CTL" dataset activetimestamp  "$((now_us + 300))000000"     >/dev/null 2>&1
    "$OT_CTL" dataset delay            "$DELAY_MS"                   >/dev/null 2>&1
    "$OT_CTL" dataset commit pending   >/dev/null 2>&1
    log "  pending dataset committed; waiting ${SETTLE_SEC}s for switchover + settle"
    sleep "$SETTLE_SEC" & wait $!
    actual=$(current_channel)
    nc=$(children_count)
    log "  post-migration: channel=$actual children=$nc"
    if [ "$actual" != "$target" ]; then
        log "  ERROR: channel did not switch to $target (still $actual)"
        return 1
    fi
}

# Resolve FROM/TO if not given
[ -z "$FROM" ] && FROM=$(current_channel)
if [ -z "$TO" ]; then
    if [ "$FROM" = "26" ]; then TO=15; else TO=26; fi
fi

log "=== Channel migration test starting ==="
log "from=$FROM to=$TO delay_ms=$DELAY_MS settle_sec=$SETTLE_SEC sample_sec=$SAMPLE_SEC"
log "current channel=$(current_channel) children=$(children_count)"

# 1) Baseline on starting channel
log "--- baseline ch${FROM} ---"
"$RT" "phase2_ch${FROM}a" "$SAMPLE_SEC" 30 </dev/null >>/tmp/range.log 2>&1
log "baseline ch${FROM} done; children=$(children_count)"

# 2) Migrate to target
log "--- migrate ch${FROM} → ch${TO} ---"
if ! migrate_to "$TO"; then
    log "ABORT: migration to ch${TO} failed; staying on $(current_channel)"
    exit 1
fi

# 3) Sample on target
log "--- sample ch${TO} ---"
"$RT" "phase2_ch${TO}" "$SAMPLE_SEC" 30 </dev/null >>/tmp/range.log 2>&1
log "ch${TO} done; children=$(children_count)"

# 4) Migrate back
log "--- migrate ch${TO} → ch${FROM} ---"
if ! migrate_to "$FROM"; then
    log "ABORT: migration back to ch${FROM} failed; on $(current_channel)"
    exit 2
fi

# 5) Control sample on starting channel (half duration is plenty)
HALF=$((SAMPLE_SEC / 2))
[ "$HALF" -lt 60 ] && HALF=60
log "--- control sample ch${FROM} (${HALF}s) ---"
"$RT" "phase2_ch${FROM}b" "$HALF" 30 </dev/null >>/tmp/range.log 2>&1
log "control ch${FROM} done; children=$(children_count)"

log "=== Channel migration test complete ==="
