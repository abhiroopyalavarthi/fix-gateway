#!/usr/bin/env bash
# The end-to-end demo:
#   1. start the gateway
#   2. CLIENT1 streams buys and CLIENT2 streams sells into AAPL at the same price
#   3. kill -9 CLIENT1 halfway; CLIENT2 keeps filling CLIENT1's resting orders
#   4. restart CLIENT1: sequence numbers resync, the gateway resends what it missed
#   5. check CLIENT1's view of every order against the gateway's, and count orders
#   6. an oversized order gets rejected by risk; fixlog explains why
#   7. the kill switch cancels everything CLIENT2 still has open
#
# Run from the repo root after building: scripts/recovery_demo.sh [build-dir]
set -euo pipefail
BIN=${1:-build}
N=2000        # orders per client
RATE1=1000    # CLIENT1 buys faster than CLIENT2 sells, so it always has
RATE2=500     # resting buys for CLIENT2 to fill while CLIENT1 is dead
S=demo        # everything this run writes goes under here

rm -rf store log "$S" && mkdir -p "$S"
mkfifo "$S/admin"
"$BIN/fix_gateway" --dump "$S/gateway_dump.txt" < "$S/admin" > "$S/gateway.out" 2>&1 &
GW=$!
exec 3> "$S/admin"          # keep the admin pipe open so we can send commands
trap 'echo quit >&3 2>/dev/null; wait $GW 2>/dev/null || true' EXIT
sleep 1

stream() {  # client side price rate; exec so that $! is fix_client itself, not a subshell
    exec "$BIN/fix_client" stream --sender "$1" --prefix "$1" --orders $N --side "$2" --price "$3" \
        --qty 100 --rate "$4" --journal "$S/$1.journal" --view "$S/$1.view"
}

echo "== CLIENT1 buys, CLIENT2 sells, $N orders each"
(stream CLIENT1 buy 338.00 $RATE1) > "$S/client1_first.out" 2>&1 &
C1=$!
(stream CLIENT2 sell 338.00 $RATE2) > "$S/client2.out" 2>&1 &
C2=$!

sleep 1.0
kill -9 $C1
wait $C1 2>/dev/null || true
echo "== kill -9 CLIENT1 after $(grep -c '^SENT' "$S/CLIENT1.journal") orders; CLIENT2 keeps filling its resting buys"
sleep 1.5
echo "== restarting CLIENT1"
(stream CLIENT1 buy 338.00 $RATE1) > "$S/client1_second.out" 2>&1
wait $C2
grep -h "resuming\|reports" "$S/client1_second.out"

echo "dump $S/gateway_dump.txt" >&3
sleep 0.5

echo
echo "== does CLIENT1 agree with the gateway about every order?"
grep '^CLIENT1 ' "$S/gateway_dump.txt" | cut -d' ' -f2- | sort > "$S/gw_client1.txt"
sort "$S/CLIENT1.view" > "$S/c1_sorted.txt"
if diff -q "$S/gw_client1.txt" "$S/c1_sorted.txt" > /dev/null; then
    echo "yes: $(wc -l < "$S/c1_sorted.txt") orders, identical status/cum/leaves/avgPx on both sides"
else
    echo "NO, differences:"; diff "$S/gw_client1.txt" "$S/c1_sorted.txt" | head; exit 1
fi
booked=$(grep -c '^CLIENT1 ' "$S/gateway_dump.txt")
unique=$(grep '^CLIENT1 ' "$S/gateway_dump.txt" | cut -d' ' -f2 | sort -u | wc -l)
echo "gateway booked $booked CLIENT1 orders, $unique distinct ClOrdIDs (expected $N): \
$([ "$booked" -eq $N ] && [ "$unique" -eq $N ] && echo 'nothing lost or duplicated' || echo MISMATCH)"

echo
echo "== sequence numbers in the gateway's CLIENT1 log"
"$BIN/fixlog" gaps log/gateway/FIX.4.4-GATEWAY-CLIENT1.messages.current.log | tail -6

echo
echo "== an order over the 50,000 share limit"
"$BIN/fix_client" send --sender CLIENT1 --id BIG-1 --side buy --qty 60000 --price 338.00
"$BIN/fixlog" why BIG-1 log/gateway/FIX.4.4-GATEWAY-CLIENT1.messages.current.log

echo
echo "== kill switch on CLIENT2"
"$BIN/fix_client" send --sender CLIENT2 --id REST-1 --side sell --qty 100 --price 343.00 > /dev/null
"$BIN/fix_client" send --sender CLIENT2 --id REST-2 --side sell --qty 100 --price 344.00 > /dev/null
echo "kill CLIENT2" >&3
sleep 0.5
"$BIN/fix_client" send --sender CLIENT2 --id REST-3 --side sell --qty 100 --price 345.00
"$BIN/fixlog" why REST-1 log/gateway/FIX.4.4-GATEWAY-CLIENT2.messages.current.log
