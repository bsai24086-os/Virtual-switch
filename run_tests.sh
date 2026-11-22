#!/bin/bash
# Test suite for the virtual switch.
#   ./run_tests.sh                     plain build
#   ./run_tests.sh ./switch_tsan       ThreadSanitizer build
#   VALGRIND=1 ./run_tests.sh          under valgrind
SW=${1:-./switch}
P=${PORT:-9300}
WRAP=""
[ -n "$VALGRIND" ] && WRAP="valgrind --leak-check=full --show-leak-kinds=definite,indirect --errors-for-leak-kinds=definite,indirect --error-exitcode=99 -q"
PASS=0; FAIL=0
SETTLE=0.2; [ -n "$VALGRIND" ] && SETTLE=2    # time for the switch to free a port after a host leaves
OUT=test_out; mkdir -p $OUT

ok()   { echo "PASS  $1"; PASS=$((PASS+1)); }
bad()  { echo "FAIL  $1"; FAIL=$((FAIL+1)); }
check(){ if eval "$2"; then ok "$1"; else bad "$1  [$2]"; fi; }
field(){ grep -o "$2=[0-9.]*" "$1" | head -1 | cut -d= -f2; }

start() {   # $1 = name, $2 = num_ports, $3 = optional config
    NAME=$1
    LOG=$OUT/$NAME.switch.txt
    $WRAP $SW $P $2 $3 > $LOG 2>&1 &
    SWPID=$!
    for _ in $(seq 100); do grep -q "switch ready" $LOG && break; sleep 0.1; done
    echo "---- $NAME ----"
}

stop() {
    kill -INT $SWPID
    wait $SWPID; local rc=$?
    check "$NAME: clean SIGINT shutdown (exit $rc)" "[ $rc -eq 0 ] && grep -q 'signal 2 received' $LOG && grep -q 'shutdown complete' $LOG"
    check "$NAME: no ThreadSanitizer reports" "! grep -q 'WARNING: ThreadSanitizer' $LOG"
    check "$NAME: socket and shm removed" "[ ! -e /tmp/vswitch.sock ] && [ ! -e /dev/shm/vswitch_stats ] && [ ! -e /dev/shm/vswitch_mac ]"
}

H=./testhost
rm -f /tmp/vswitch.log mac_dump.txt

# ------------------------------------------------------------------ #
start forwarding 4
$H $P 1 recv 3 > $OUT/a1.txt & A1=$!; sleep 0.3
$H $P 2 recv 3 > $OUT/a2.txt & A2=$!; sleep 0.3
sleep 0.3
./swctl show mac > $OUT/showmac.txt
$H $P 3 send 2 20 1 > /dev/null
wait $A1 $A2
check "unicast reaches only its destination (h2=20)" "[ $(field $OUT/a2.txt total) -eq 20 ]"
check "unicast does not leak to h1 (h1=0)"           "[ $(field $OUT/a1.txt total) -eq 0 ]"

$H $P 1 recv 3 > $OUT/b1.txt & B1=$!; sleep 0.3
$H $P 2 recv 3 > $OUT/b2.txt & B2=$!; sleep 0.3
$H $P 3 send bc 20 1 > /dev/null
wait $B1 $B2
check "broadcast reaches h1 (20)" "[ $(field $OUT/b1.txt total) -eq 20 ]"
check "broadcast reaches h2 (20)" "[ $(field $OUT/b2.txt total) -eq 20 ]"
check "swctl show mac lists learned MACs" "grep -q '02:00:00:00:00:02' $OUT/showmac.txt"
./swctl show ports > $OUT/showports.txt
check "swctl show ports answers" "grep -q FRAMES_IN $OUT/showports.txt"
./swctl clear mac | grep -q cleared && ok "swctl clear mac" || bad "swctl clear mac"
kill -USR1 $SWPID; sleep 0.5
check "SIGUSR1 dumps the MAC table" "[ -s mac_dump.txt ]"
stop

# ------------------------------------------------------------------ #
start slow_host 4
$H $P 9 stall 14 > /dev/null & STALL=$!; sleep 0.3
$H $P 2 recv 9 > $OUT/c2.txt & C2=$!; sleep 0.3
PAYLOAD_SIZE=1400 $H $P 3 send bc 3000 1 200 > $OUT/c3.txt
wait $C2
./swctl show ports > $OUT/c_ports.txt
GOT=$(field $OUT/c2.txt total)
DROP=$(awk '$1==0{print $4}' $OUT/c_ports.txt)
check "fast host gets everything despite a frozen host ($GOT/3000)" "[ $GOT -eq 3000 ]"
check "frozen host's queue drops and counts ($DROP drops)" "[ ${DROP:-0} -gt 0 ]"
kill $STALL 2>/dev/null; sleep 0.5
stop

# ------------------------------------------------------------------ #
start port_limit 2
$H $P 1 stall 9 > /dev/null & D1=$!; sleep 0.3
$H $P 2 stall 9 > /dev/null & D2=$!; sleep 0.3
T0=$(date +%s.%N)
$H $P 3 full > $OUT/d3.txt
T1=$(date +%s.%N)
WAITED=$(echo "$T1 - $T0" | bc)
check "extra host gets 'switch full'" "grep -q 'switch full' $OUT/d3.txt"
check "after waiting about 5 s ($WAITED s)" "[ \$(echo '$WAITED > 4.5 && $WAITED < 7' | bc) -eq 1 ]"
kill $D1 $D2 2>/dev/null; wait $D1 $D2 2>/dev/null
stop

# ------------------------------------------------------------------ #
printf 'rate_limit 20\nlow_aging_ms 5000\nwrr_weights 5 3 1\n' > $OUT/qos.conf
start qos 4 $OUT/qos.conf
$H $P 2 recv 9 > $OUT/e2.txt & E2=$!; sleep 0.3
$H $P 3 mix 2 40 > /dev/null
wait $E2
SEQ=$(grep -o 'SEQ=.*' $OUT/e2.txt | cut -d= -f2)
TAIL=${SEQ:20:54}
echo "      order after the 20-frame burst: $TAIL"
check "WRR 5:3:1 pattern (HHHHHNNNL repeats)" "echo $TAIL | grep -q 'HHHHHNNNLHHHHHNNNL'"
RATE=$(field $OUT/e2.txt rate_1to3s)
check "rate limit holds at 20 fps (measured $RATE)" "[ \$(echo '$RATE >= 17 && $RATE <= 23' | bc) -eq 1 ]"
TOTAL=$(field $OUT/e2.txt total)
check "nothing lost while rate limited ($TOTAL/120)" "[ $TOTAL -eq 120 ]"
L_BEFORE=$(echo "${SEQ:20:45}" | tr -cd L | wc -c)

printf 'rate_limit 20\nlow_aging_ms 100\nwrr_weights 5 3 1\n' > $OUT/qos.conf
kill -HUP $SWPID; sleep 0.5
check "SIGHUP reloads the config" "grep -q 'low_aging_ms=100' $LOG"
$H $P 2 recv 9 > $OUT/f2.txt & F2=$!; sleep 0.3
$H $P 3 mix 2 40 > /dev/null
wait $F2
SEQ2=$(grep -o 'SEQ=.*' $OUT/f2.txt | cut -d= -f2)
L_AFTER=$(echo "${SEQ2:20:45}" | tr -cd L | wc -c)
echo "      with aging 100 ms:              ${SEQ2:20:54}"
check "aging moves low frames up ($L_BEFORE -> $L_AFTER of 45)" "[ $L_AFTER -gt $L_BEFORE ]"
stop

# ------------------------------------------------------------------ #
start mirror 4
$H $P 1 recv 5 > $OUT/g1.txt & G1=$!; sleep 0.3
$H $P 5 recv 5 > $OUT/g5.txt & G5=$!; sleep 0.3
START_DELAY_MS=1000 $H $P 3 send 1 20 1 > /dev/null & G3=$!; sleep 0.4
./swctl mirror 2 1 > $OUT/g_cmd.txt
wait $G1 $G5 $G3
./swctl show ports > $OUT/g_ports.txt
check "swctl mirror accepted" "grep -q 'mirroring to port 1' $OUT/g_cmd.txt"
check "destination still gets 20" "[ $(field $OUT/g1.txt total) -eq 20 ]"
check "sniffer gets a copy of all 20" "[ $(field $OUT/g5.txt total) -eq 20 ]"
check "mirrored counter is 20" "[ \"\$(awk '\$1==2{print \$5}' $OUT/g_ports.txt)\" = 20 ]"
stop

# ------------------------------------------------------------------ #
start robustness 4
( exec 3<>/dev/tcp/127.0.0.1/$P; printf '\x00\x10\x02\x00' >&3; sleep 0.3; exec 3>&- ) 2>/dev/null
sleep 0.5
$H $P 7 spoof 12 > $OUT/h7.txt
check "port security shuts a port with > 10 MACs" "grep -q 'closed_by_switch=yes' $OUT/h7.txt"
$H $P 1 recv 2 > $OUT/h1.txt & HH=$!; sleep 0.3
$H $P 2 send 1 5 1 > /dev/null
wait $HH
check "switch still forwards after half a frame + spoofing" "[ $(field $OUT/h1.txt total) -eq 5 ]"
check "log file has timestamps" "grep -qE '^\[[0-9-]+ [0-9:]+\]' /tmp/vswitch.log"
stop

# ------------------------------------------------------------------ #
start vlan 5
./swctl vlan 1 20 > /dev/null; ./swctl vlan 3 20 > /dev/null     # while free
$H $P 1 recv 11 > $OUT/v1.txt & V1=$!; sleep 0.3                 # port 0, VLAN 1
$H $P 2 recv 11 > $OUT/v2.txt & V2=$!; sleep 0.3                 # port 1, VLAN 20
$H $P 3 recv 11 > $OUT/v3.txt & V3=$!; sleep 0.3                 # port 2, VLAN 1
$H $P 4 send bc 10 1 > /dev/null                                 # port 3, VLAN 20
sleep $SETTLE
./swctl vlan 0 20 > $OUT/v_cmd.txt                               # live, host connected
$H $P 4 send bc 10 1 > /dev/null
sleep $SETTLE
$H $P 5 send 3 5 1 > /dev/null                                   # port 3 (VLAN 20), dst is in VLAN 1
wait $V1 $V2 $V3
check "VLAN 20 host got 10 + 10 broadcasts + 5 flooded (25)" "[ $(field $OUT/v2.txt total) -eq 25 ]"
check "VLAN 1 host never saw VLAN 20 traffic, even a unicast to it (0)" "[ $(field $OUT/v3.txt total) -eq 0 ]"
check "live 'vlan 0 20' moved host A into VLAN 20 (A got 10+5)" "[ $(field $OUT/v1.txt total) -eq 15 ]"
check "swctl vlan replies" "grep -q 'VLAN 20' $OUT/v_cmd.txt"
./swctl vlan 0 5000 | grep -q invalid && ok "swctl rejects VLAN 5000" || bad "swctl rejects VLAN 5000"
./swctl show ports > $OUT/v_ports.txt
check "show ports lists VLAN and state" "grep -q 'VLAN' $OUT/v_ports.txt && grep -q access $OUT/v_ports.txt"
stop

# ------------------------------------------------------------------ #
start trunk 4
./swctl vlan 1 20 > /dev/null; ./swctl trunk 2 on > /dev/null
$H $P 1 recv 11 > $OUT/t1.txt & T1=$!; sleep 0.3                 # port 0, VLAN 1
$H $P 2 recv 11 > $OUT/t2.txt & T2=$!; sleep 0.3                 # port 1, VLAN 20
$H $P 3 recv 11 > $OUT/t3.txt & T3=$!; sleep 0.3                 # port 2, trunk
$H $P 4 send bc 6 1 > /dev/null                                  # port 3, VLAN 1
./swctl vlan 3 20 > /dev/null
$H $P 4 send bc 6 1 > /dev/null                                  # port 3, VLAN 20
./swctl trunk 3 on > /dev/null
VLAN=20 $H $P 4 send bc 6 1 > /dev/null                          # port 3 is a trunk, vlan field 20
wait $T1 $T2 $T3
check "VLAN 1 host got only VLAN 1 broadcasts (6)"      "[ $(field $OUT/t1.txt total) -eq 6 ]"
check "VLAN 20 host got 6 + 6 (own move + trunk frames)" "[ $(field $OUT/t2.txt total) -eq 12 ]"
check "trunk port carries every VLAN (18)"               "[ $(field $OUT/t3.txt total) -eq 18 ]"
check "frames leaving the trunk keep their vlan field (20)" "[ $(field $OUT/t3.txt lastvlan) -eq 20 ]"
stop

# ------------------------------------------------------------------ #
start admin 3
$H $P 1 recv 9 > $OUT/i1.txt & I1=$!; sleep 0.3                  # port 0
$H $P 2 recv 9 > $OUT/i2.txt & I2=$!; sleep 0.3                  # port 1
./swctl shutdown 0 > $OUT/i_cmd.txt
check "swctl shutdown replies once the port is free" "grep -q 'port 0 shut down' $OUT/i_cmd.txt"
sleep 0.3
check "connected host was cut off" "! kill -0 $I1 2>/dev/null"
./swctl show ports > $OUT/i_ports.txt
check "show ports says port 0 is down" "[ \"\$(awk '\$1==0{print \$NF}' $OUT/i_ports.txt)\" = down ]"
$H $P 3 stall 4 > /dev/null & I3=$!; sleep 0.3                   # takes port 2
T0=$(date +%s.%N)
$H $P 4 full > $OUT/i_full.txt
T1=$(date +%s.%N)
check "no usable port: 'switch full' straight away" "grep -q 'switch full' $OUT/i_full.txt && [ \$(echo \"$T1 - $T0 < 2\" | bc) -eq 1 ]"
./swctl up 0 | grep -q "port 0 up" && ok "swctl up" || bad "swctl up"
$H $P 5 send bc 3 1 > /dev/null                                  # must land on port 0
wait $I2
check "port 0 is usable again after 'up' (B got 3)" "[ $(field $OUT/i2.txt total) -eq 3 ]"
./swctl shutdown 99 | grep -q invalid && ok "shutdown 99 rejected" || bad "shutdown 99 rejected"
kill $I3 2>/dev/null; wait $I3 2>/dev/null
stop

# ------------------------------------------------------------------ #
# supervisor: a crash is survived and the MAC table is kept (plain build only)
echo "---- supervisor ----"
if [ -x ./supervisor ] && [ "$SW" = ./switch ] && [ -z "$VALGRIND" ]; then
    SLOG=$OUT/supervisor.txt
    ./supervisor $P 4 > $SLOG 2>&1 &
    SUP=$!
    for _ in $(seq 100); do [ "$(grep -c 'switch ready' $SLOG)" -ge 1 ] && break; sleep 0.1; done
    $H $P 1 recv 30 > /dev/null & S1=$!; sleep 0.3
    $H $P 2 recv 30 > /dev/null & S2=$!; sleep 0.3
    # wait until both hosts are learned (a slow disk can take a while)
    for _ in $(seq 100); do
        ./swctl show mac > $OUT/sup_before.txt
        [ "$(grep -c '^02:' $OUT/sup_before.txt)" -ge 2 ] && break
        sleep 0.1
    done
    check "before the crash both MACs are learned" "grep -q '02:00:00:00:00:01' $OUT/sup_before.txt && grep -q '02:00:00:00:00:02' $OUT/sup_before.txt"
    OLDPID=$(pgrep -P $SUP -x switch)
    kill -9 $OLDPID
    for _ in $(seq 100); do [ "$(grep -c 'switch ready' $SLOG)" -ge 2 ] && break; sleep 0.1; done
    NEWPID=$(pgrep -P $SUP -x switch)
    check "supervisor restarted the switch (new pid)" "[ -n \"$NEWPID\" ] && [ \"$NEWPID\" != \"$OLDPID\" ]"
    check "restart reattached the MAC table" "grep -q 'reattached /vswitch_mac, kept 2 entries' $SLOG"
    ./swctl show mac > $OUT/sup_after.txt
    check "MACs learned before the crash are still there" "grep -q '02:00:00:00:00:01' $OUT/sup_after.txt && grep -q '02:00:00:00:00:02' $OUT/sup_after.txt"
    kill $S1 $S2 2>/dev/null; wait $S1 $S2 2>/dev/null
    $H $P 3 recv 3 > $OUT/sup_r.txt & S3=$!; sleep 0.3
    $H $P 4 send bc 5 1 > /dev/null
    wait $S3
    check "the restarted switch forwards (got 5)" "[ $(field $OUT/sup_r.txt total) -eq 5 ]"
    kill -TERM $SUP
    wait $SUP; rc=$?
    check "SIGTERM stops the supervisor with exit 0" "[ $rc -eq 0 ]"
    check "no switch left running" "! pgrep -x switch > /dev/null"
    check "clean stop removed the MAC shm" "[ ! -e /dev/shm/vswitch_mac ]"

    # a switch that dies at once (bad port) must not be restarted forever
    T0=$(date +%s.%N)
    ./supervisor 70000 4 > $OUT/sup_loop.txt 2>&1; rc=$?
    T1=$(date +%s.%N)
    check "crash loop: supervisor gives up (exit 1)" "[ $rc -eq 1 ] && grep -q 'giving up' $OUT/sup_loop.txt"
    check "crash loop: gave up within 5 s" "[ \$(echo \"$T1 - $T0 < 5\" | bc) -eq 1 ]"
else
    echo "skipped (plain ./switch build only)"
fi

echo
echo "RESULT: $PASS passed, $FAIL failed"
[ $FAIL -eq 0 ]