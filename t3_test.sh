#!/bin/bash
# Tier 3 trunk test: two switches joined by two pipes.
# usage: ./t3_test.sh [switch_binary]
BIN=${1:-./switch}
PA=9200; PB=9201
P=0; F=0
ok()  { echo "PASS  $1"; P=$((P+1)); }
bad() { echo "FAIL  $1"; F=$((F+1)); }
check() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }
A() { VSWITCH_NAME=a ./swctl "$@"; }
B() { VSWITCH_NAME=b ./swctl "$@"; }

pkill -x switch; pkill -x twoswitch; pkill -x testhost; pkill -x switch_tsan; sleep 0.3
./twoswitch $PA $PB 4 $BIN > /tmp/t3_log.txt 2>&1 & TW=$!
for i in $(seq 100); do [ "$(grep -c 'switch ready' /tmp/t3_log.txt)" = 2 ] && break; sleep 0.1; done
check "both switches started" '[ "$(grep -c "switch ready" /tmp/t3_log.txt)" = 2 ]'
check "A and B have their own control sockets" '[ -S /tmp/vswitch.sock_a ] && [ -S /tmp/vswitch.sock_b ]'
check "A and B have their own shared memory" '[ -e /dev/shm/vswitch_mac_a ] && [ -e /dev/shm/vswitch_mac_b ]'
check "port 0 is the trunk on A" 'A show ports | grep -q "^0 .*trunk"'
check "port 0 is the trunk on B" 'B show ports | grep -q "^0 .*trunk"'

echo "---- unicast across the trunk ----"
./testhost $PB 11 recv 3 > /tmp/t3_b.txt & R=$!
sleep 0.5
./testhost $PA 10 send 11 200 1 300 > /dev/null
wait $R
check "host 0A on A -> host 0B on B: 200 of 200" 'grep -q "N=200" /tmp/t3_b.txt'
check "A learned 0B behind its trunk (port 0)" 'A show mac | grep -i "00:0b" | grep -qw 0'
check "B learned 0A behind its trunk (port 0)" 'B show mac | grep -i "00:0a" | grep -qw 0'

echo "---- the other way ----"
./testhost $PA 10 recv 3 > /tmp/t3_a.txt & R=$!
sleep 0.5
./testhost $PB 11 send 10 150 1 300 > /dev/null
wait $R
check "host 0B on B -> host 0A on A: 150 of 150" 'grep -q "N=150" /tmp/t3_a.txt'

echo "---- broadcast reaches both switches ----"
./testhost $PA 12 recv 3 > /tmp/t3_c.txt & R1=$!
./testhost $PB 11 recv 3 > /tmp/t3_b.txt & R2=$!
sleep 0.5
./testhost $PA 10 send bc 50 1 300 > /dev/null
wait $R1 $R2
check "broadcast: host 0C on the same switch got 50" 'grep -q "N=50" /tmp/t3_c.txt'
check "broadcast: host 0B on the other switch got 50" 'grep -q "N=50" /tmp/t3_b.txt'

echo "---- VLANs across the trunk ----"
# the trunk is port 0, so the first host on each switch gets port 1
A vlan 1 2 > /dev/null                       # A's host in VLAN 2, B's in VLAN 1
./testhost $PB 11 recv 3 > /tmp/t3_b.txt & R=$!
sleep 0.4
./testhost $PA 10 send 11 40 1 300 > /dev/null
wait $R
check "VLAN 2 on A does not reach VLAN 1 on B ($(grep -o 'N=[0-9]*' /tmp/t3_b.txt))" 'grep -q "N=0 " /tmp/t3_b.txt'
B vlan 1 2 > /dev/null                       # now both in VLAN 2
./testhost $PB 11 recv 3 > /tmp/t3_b.txt & R=$!
sleep 0.4
./testhost $PA 10 send 11 40 1 300 > /dev/null
wait $R
check "VLAN 2 on A reaches VLAN 2 on B ($(grep -o 'N=[0-9]*' /tmp/t3_b.txt))" 'grep -q "N=40 " /tmp/t3_b.txt'
check "the frame crossed the trunk tagged VLAN 2" 'grep -q "lastvlan=2" /tmp/t3_b.txt'
A vlan 1 1 > /dev/null; B vlan 1 1 > /dev/null

echo "---- one switch dies ----"
PB_PID=$(grep -o "B pid [0-9]*" /tmp/t3_log.txt | awk '{print $3}')
kill -9 $PB_PID
for i in $(seq 30); do grep -q "A| \[port 0\] removed" /tmp/t3_log.txt && break; sleep 0.1; done
check "A sees the trunk go down" 'grep -q "A| \[port 0\] removed" /tmp/t3_log.txt'
check "A's port 0 is no longer a trunk" 'A show ports | grep "^0 " | grep -q access'
./testhost $PA 12 recv 2 > /tmp/t3_c.txt & R=$!
sleep 0.4
./testhost $PA 10 send 12 30 1 300 > /dev/null
wait $R
check "A still forwards on its own: 30 of 30" 'grep -q "N=30" /tmp/t3_c.txt'

echo "---- clean stop ----"
kill -INT $TW
wait $TW; RC=$?
check "A shut down cleanly" 'grep -q "A| \[switch\] shutdown complete" /tmp/t3_log.txt'
check "twoswitch reported B killed by signal 9" 'grep -q "switch B killed by signal 9" /tmp/t3_log.txt'
check "A's socket and shm removed" '[ ! -e /tmp/vswitch.sock_a ] && [ ! -e /dev/shm/vswitch_mac_a ] && [ ! -e /dev/shm/vswitch_stats_a ]'
rm -f /tmp/vswitch.sock_b /dev/shm/vswitch_mac_b /dev/shm/vswitch_stats_b   # B was killed -9

echo "---- both stop with Ctrl-C ----"
./twoswitch $PA $PB 4 $BIN > /tmp/t3_log2.txt 2>&1 & TW=$!
for i in $(seq 100); do [ "$(grep -c 'switch ready' /tmp/t3_log2.txt)" = 2 ] && break; sleep 0.1; done
./testhost $PB 11 recv 2 > /dev/null & R=$!; sleep 0.3; ./testhost $PA 10 send 11 500 1 > /dev/null; wait $R
kill -INT $TW; wait $TW; RC=$?
check "twoswitch exits 0 after Ctrl-C" '[ $RC = 0 ]'
check "both switches exited with 0" '[ "$(grep -c "exited with 0" /tmp/t3_log2.txt)" = 2 ]'
check "nothing left in /dev/shm or /tmp" '! ls /dev/shm/vswitch_* /tmp/vswitch.sock_* 2>/dev/null | grep -q .'
check "no ThreadSanitizer reports" '! grep -q "ThreadSanitizer" /tmp/t3_log.txt /tmp/t3_log2.txt'

echo; echo "RESULT: $P passed, $F failed"