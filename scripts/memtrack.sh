#!/usr/bin/env bash
# memtrack.sh -- sample RSS / VSZ of a process over time.
#
# Usage:
#   ./scripts/memtrack.sh <pid> <out.csv> [interval_seconds]
#
# Writes CSV: timestamp_s,vmrss_kb,vmsize_kb
# Stops automatically when <pid> exits.

set -euo pipefail

if [[ $# -lt 2 ]]; then
    echo "Usage: $0 <pid> <out.csv> [interval_seconds]" >&2
    exit 1
fi

PID="$1"
OUT="$2"
INTERVAL="${3:-0.2}"

if [[ ! -d "/proc/$PID" ]]; then
    echo "no such pid: $PID" >&2
    exit 1
fi

echo "timestamp_s,vmrss_kb,vmsize_kb" > "$OUT"

T0=$(date +%s.%N)
while [[ -d "/proc/$PID" ]]; do
    if ! status=$(cat "/proc/$PID/status" 2>/dev/null); then
        break
    fi
    rss=$(echo "$status" | awk '/^VmRSS:/ {print $2}')
    vsz=$(echo "$status" | awk '/^VmSize:/ {print $2}')
    now=$(date +%s.%N)
    elapsed=$(awk -v a="$now" -v b="$T0" 'BEGIN{printf "%.3f", a-b}')
    echo "$elapsed,${rss:-0},${vsz:-0}" >> "$OUT"
    sleep "$INTERVAL"
done

# quick summary on stderr
awk -F, 'NR>1 {if($2>max)max=$2; sum+=$2; n++} END {
    if (n) printf "samples=%d  rss_peak=%d kB  rss_avg=%.0f kB\n", n, max, sum/n
}' "$OUT" >&2
