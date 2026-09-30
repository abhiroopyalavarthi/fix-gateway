#!/usr/bin/env bash
# Order-to-ack latency and throughput through the full FIX path, with the
# gateway and client persisting to disk (the real setup) and without.
# Run from the repo root after building: scripts/bench.sh [build-dir] [orders]
set -euo pipefail
BIN=${1:-build}
N=${2:-50000}

run() {  # $1 = store kind, $2 = log kind
    rm -rf store log
    "$BIN/fix_gateway" --store "$1" --log "$2" > /dev/null 2>&1 &
    local gw=$!
    sleep 1
    echo "### store=$1 log=$2"
    "$BIN/fix_client" bench --sender BENCH --orders "$N" --mode pingpong --store "$1" --log "$2"
    "$BIN/fix_client" bench --sender BENCH --orders "$N" --mode burst --window 64 --store "$1" --log "$2" \
        | grep throughput
    kill -INT $gw; wait $gw 2>/dev/null || true
    echo
}

echo "machine: $(uname -sm), $(sysctl -n machdep.cpu.brand_string 2>/dev/null || grep -m1 'model name' /proc/cpuinfo | cut -d: -f2)"
run file file
run memory none
