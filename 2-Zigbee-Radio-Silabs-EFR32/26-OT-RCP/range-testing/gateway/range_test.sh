#!/bin/sh
# range_test.sh — Thread mesh range-test CSV sampler
#
# Polls `ot-ctl neighbor table` at fixed intervals and logs one CSV row per
# attached child per cycle. One invocation = one experimental palier (a
# single TX power, channel, orientation, etc.). Between paliers, change the
# variable under test and start a fresh invocation with a different label.
#
# Designed to be portable across any host with `ot-ctl` available, and to
# work inside the BusyBox userland of the Lidl Silvercrest gateway (no
# `tr`, `find`, `pkill`, etc. — only POSIX-y substitutes).
#
# Usage:
#   range_test.sh <label> <duration_sec> [interval_sec]
#
# Defaults: interval_sec = 60.
#
# Output:
#   $LOG_DIR/range_<label>.csv
#
# Environment variables:
#   OT_CTL    path to ot-ctl (default: ot-ctl on $PATH)
#   LOG_DIR   directory for CSV output (default: /userdata/log)
#
# CSV header lines (commented):
#   # label=… duration=… interval=…
#   # txpower=… channel=… panid=… state=…
#   # start=<unix> (utc=<human>)
#
# CSV column header:
#   ts,label,rloc,role,age,avg_rssi,last_rssi,lq_in,ext_mac
#
# Notes:
# - "ts" is wall-clock unix time at the start of the polling cycle.
# - "rloc" / "ext_mac" come from `ot-ctl neighbor table`. The ext_mac is
#   stable for Zigbee-style devices but can rotate per-session for some
#   Matter implementations — pin per-sensor identity at the application
#   layer (e.g. matter-server) if you need a long-term identifier.
# - "avg_rssi" / "last_rssi" are gateway-side measurements of the uplink
#   only. The downlink (gateway → device) is not directly observable.
# - Cancelling the script (SIGINT/SIGTERM) leaves a partial CSV in place;
#   the comment header records the conditions at start so the partial file
#   is still self-describing.

set -u

LABEL="${1:-}"
DURATION="${2:-}"
INTERVAL="${3:-60}"

if [ -z "$LABEL" ] || [ -z "$DURATION" ]; then
    echo "Usage: $0 <label> <duration_sec> [interval_sec]" >&2
    exit 1
fi

OT_CTL="${OT_CTL:-ot-ctl}"
LOG_DIR="${LOG_DIR:-/userdata/log}"
OUT="$LOG_DIR/range_${LABEL}.csv"
mkdir -p "$LOG_DIR"

# Single-line query helper.
ot_query() {
    timeout 3 "$OT_CTL" "$@" 2>/dev/null | sed 's/\r$//' | grep -v '^Done$' | head -1
}

TXPWR=$("$OT_CTL" txpower 2>/dev/null | sed 's/\r$//' | grep -oE '[0-9-]+ dBm' | head -1)
CHAN=$(ot_query channel)
PANID=$(ot_query panid)
STATE=$(ot_query state)
START_TS=$(date -u +%s)

{
    echo "# label=${LABEL} duration=${DURATION}s interval=${INTERVAL}s"
    echo "# txpower=${TXPWR} channel=${CHAN} panid=${PANID} state=${STATE}"
    echo "# start=${START_TS} (utc=$(date -u))"
    echo "ts,label,rloc,role,age,avg_rssi,last_rssi,lq_in,ext_mac"
} > "$OUT"

echo "range_test: writing to $OUT"
echo "  txpower=$TXPWR channel=$CHAN state=$STATE"
echo "  duration=${DURATION}s interval=${INTERVAL}s"

END=$((START_TS + DURATION))
NOW=$START_TS

while [ "$NOW" -lt "$END" ]; do
    ts=$(date +%s)
    timeout 3 "$OT_CTL" neighbor table 2>/dev/null | sed 's/\r$//' | \
        awk -F'|' -v ts="$ts" -v lbl="$LABEL" '
            /^\| *[CR] *\|/ {
                for (i=1; i<=NF; i++) gsub(/^[[:space:]]+|[[:space:]]+$/, "", $i)
                print ts "," lbl "," $3 "," $2 "," $4 "," $5 "," $6 "," $7 "," $11
            }
        ' >> "$OUT"

    NOW=$(date +%s)
    REMAIN=$((END - NOW))
    [ "$REMAIN" -le 0 ] && break
    SLEEP=$INTERVAL
    [ "$SLEEP" -gt "$REMAIN" ] && SLEEP=$REMAIN
    sleep "$SLEEP" & wait $!
done

LINES=$(grep -c '^[0-9]' "$OUT" 2>/dev/null || echo 0)
echo "range_test: done. ${LINES} samples in $OUT"
