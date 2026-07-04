#!/usr/bin/env bash
# run_bench.sh -- start the server, sample its memory, run the benchmark.
#
# Outputs into ./bench_out/:
#   bench.csv     -- benchmark phase results (ops/sec + latency percentiles)
#   memtrack.csv  -- RSS samples over the run
#   summary.txt   -- one-page summary combining both
#
# Usage:
#   ./scripts/run_bench.sh [extra benchmark args...]
# Example:
#   ./scripts/run_bench.sh -c 100 -n 200000 -P 32

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SERVER="$ROOT/bin/server"
BENCH="$ROOT/bin/benchmark"
MEMTRACK="$ROOT/scripts/memtrack.sh"
OUT="$ROOT/bench_out"

if [[ ! -x "$SERVER" || ! -x "$BENCH" ]]; then
    echo "binaries missing -- run \`make\` from $ROOT first" >&2
    exit 1
fi

mkdir -p "$OUT"
BENCH_CSV="$OUT/bench.csv"
MEM_CSV="$OUT/memtrack.csv"
SUMMARY="$OUT/summary.txt"
SERVER_LOG="$OUT/server.log"

cleanup() {
    if [[ -n "${SERVER_PID:-}" ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    if [[ -n "${MEM_PID:-}" ]] && kill -0 "$MEM_PID" 2>/dev/null; then
        kill "$MEM_PID" 2>/dev/null || true
        wait "$MEM_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT INT TERM

# -- start server
"$SERVER" > "$SERVER_LOG" 2>&1 &
SERVER_PID=$!
echo "server pid=$SERVER_PID"

# wait for it to bind 1234
for _ in $(seq 1 50); do
    if (echo > /dev/tcp/127.0.0.1/1234) 2>/dev/null; then break; fi
    sleep 0.1
done

# -- start memory sampler
"$MEMTRACK" "$SERVER_PID" "$MEM_CSV" 0.1 &
MEM_PID=$!

# -- run benchmark
echo "running benchmark: $* ..."
"$BENCH" "$@" > "$BENCH_CSV"
BENCH_RC=$?

# stop sampler before killing server, so the CSV is closed cleanly
kill "$MEM_PID" 2>/dev/null || true
wait "$MEM_PID" 2>/dev/null || true
unset MEM_PID

# -- summary
{
    echo "==== toyredis benchmark summary ===="
    echo "date: $(date -Iseconds)"
    echo "host: $(uname -srm)"
    if command -v nproc >/dev/null; then echo "cpus: $(nproc)"; fi
    echo
    echo "-- throughput / latency (per phase) --"
    if command -v column >/dev/null; then
        column -t -s, < "$BENCH_CSV"
    else
        # fall back to a simple aligned print
        awk -F, 'BEGIN { OFS="  " } { for (i=1;i<=NF;i++) printf "%-12s", $i; print "" }' "$BENCH_CSV"
    fi
    echo
    echo "-- memory (server RSS in kB, sampled every 100ms) --"
    awk -F, 'NR>1 {
        if($2>max) max=$2
        if(min==""||$2<min) min=$2
        sum+=$2; n++
    } END {
        if (n) printf "samples=%d  rss_min=%d  rss_avg=%.0f  rss_peak=%d\n", n, min, sum/n, max
        else printf "no samples\n"
    }' "$MEM_CSV"
} | tee "$SUMMARY"

echo
echo "files written to: $OUT/"
ls -la "$OUT"
exit $BENCH_RC
