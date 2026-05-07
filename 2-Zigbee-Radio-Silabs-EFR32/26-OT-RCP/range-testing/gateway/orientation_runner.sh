#!/bin/sh
# orientation_runner.sh — paced orientation-test runner
#
# Runs a series of `range_test.sh` paliers separated by user-confirmed
# physical rotations (gateway antenna, sensor body, …). Each palier waits
# for an "ack" before starting, so the operator can rotate the device
# under test, then confirm and let the sampler run.
#
# Usage:
#   orientation_runner.sh <subject> <orient_list> [stab_sec=120] [sample_sec=480]
#
# subject:      free text identifying what is being rotated (used to
#               build per-palier CSV labels). Examples: "gateway",
#               "myggbett_5202".
# orient_list:  space- or comma-separated list of orientation names.
#               Examples: "A B C", "horiz_par,horiz_perp,vert_face,vert_back".
#
# Defaults: stab_sec=120, sample_sec=480.
#
# Operator workflow:
# 1. Place the subject in the first orientation. Run this script.
# 2. The script writes a marker file telling you to ACK the orientation.
# 3. Touch the ack file; the script waits stab_sec then samples sample_sec.
# 4. When sampling is done, a "rotate now" marker appears. Rotate the
#    subject to the next orientation, then ACK again. Repeat until all
#    paliers are sampled.
#
# Output:
#   $LOG_DIR/range_<subject>_orient<name>.csv    one per palier
#   /tmp/orientation_runner.log                  summary log
#   /tmp/orientation_ack                         operator ack file
#
# To ACK an orientation from another shell:
#   touch /tmp/orientation_ack
#
# Environment variables:
#   OT_CTL   path to ot-ctl     (default: ot-ctl on $PATH)
#   RT       path to range_test (default: range_test.sh on $PATH)
#
# Notes:
# - This runner is intentionally synchronous and operator-driven so that
#   the recorded data is always tied to a known physical pose. Don't
#   automate ACK away — that defeats the audit trail.

set -u

SUBJECT="${1:-}"
ORIENT_LIST="${2:-}"
shift 2 2>/dev/null || true

STAB_SEC=120
SAMPLE_SEC=480
for arg in "$@"; do
    case "$arg" in
        stab_sec=*)   STAB_SEC="${arg#stab_sec=}" ;;
        sample_sec=*) SAMPLE_SEC="${arg#sample_sec=}" ;;
        *)            echo "unknown arg: $arg" >&2; exit 2 ;;
    esac
done

if [ -z "$SUBJECT" ] || [ -z "$ORIENT_LIST" ]; then
    echo "Usage: $0 <subject> <orient_list> [stab_sec=N] [sample_sec=N]" >&2
    echo "Example: $0 myggbett_5202 'A B C1 C2'" >&2
    exit 1
fi

OT_CTL="${OT_CTL:-ot-ctl}"
RT="${RT:-range_test.sh}"
SUMMARY=/tmp/orientation_runner.log
ACK=/tmp/orientation_ack
: > "$SUMMARY"

log() {
    echo "[$(date)] $*" | tee -a "$SUMMARY"
}

# Normalise list: replace commas with spaces
ORIENT_LIST=$(echo "$ORIENT_LIST" | sed 's/,/ /g')

log "=== orientation_runner: subject=$SUBJECT paliers=$ORIENT_LIST ==="
log "stab_sec=$STAB_SEC sample_sec=$SAMPLE_SEC"

for o in $ORIENT_LIST; do
    label="${SUBJECT}_orient${o}"
    log "--- palier $o (label=$label) ---"
    log "    place subject in orientation '$o', then 'touch $ACK' to start"

    rm -f "$ACK"
    while [ ! -f "$ACK" ]; do
        sleep 5 & wait $!
    done
    rm -f "$ACK"
    log "    ACK received; stabilising ${STAB_SEC}s"
    sleep "$STAB_SEC" & wait $!

    log "    sampling ${SAMPLE_SEC}s"
    "$RT" "$label" "$SAMPLE_SEC" 30 </dev/null >>/tmp/range.log 2>&1
    log "    palier $o done"
done

log "=== orientation_runner complete ==="
