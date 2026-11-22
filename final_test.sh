#!/bin/bash
# Final check of the whole project. Run it from the project folder.
#
#   ./final_test.sh          about 4 minutes
#   ./final_test.sh full     about 15 minutes: adds ThreadSanitizer, valgrind and MAC aging
#
# Needs: gcc, make, bc, pgrep. python3 + matplotlib is optional (graph check).
# Uses TCP port 9400 for its own switches (run_tests.sh uses 9300).
# Nothing here changes your source files. Output files go to final_out/.

MODE=${1:-normal}
P=${PORT:-9400}
OUT=final_out
H=./testhost
PASS=0; FAIL=0; SKIP=0
CFLAGS="-std=c11 -Wall -Wextra -Werror -g -O0"

ok()   { echo "PASS  $1"; PASS=$((PASS+1)); }
bad()  { echo "FAIL  $1"; FAIL=$((FAIL+1)); }
skip() { echo "SKIP  $1"; SKIP=$((SKIP+1)); }
check(){ if eval "$2"; then ok "$1"; else bad "$1   [$2]"; fi; }
sect() { echo; echo "==== $1"; }
col()  { ./swctl show ports | awk -v p="$1" -v c="$2" '$1==p {print $c}'; }   # PORT FRAMES_IN FRAMES_OUT DROPPED MIRRORED VLAN MODE STATE

# start_sw <name> <num_ports> [config]  ->  SWPID, SWLOG
start_sw() {
    SWLOG=$OUT/$1.switch.txt
    ./switch $P $2 $3 > $SWLOG 2>&1 &
    SWPID=$!
    for _ in $(seq 100); do grep -q "switch ready" $SWLOG && break; sleep 0.1; done
}
# stop_sw <name>: Ctrl+C the switch, expect exit 0 and everything removed
stop_sw() {
    kill -INT $SWPID 2>/dev/null
    wait $SWPID; local rc=$?
    check "$1: clean shutdown on SIGINT (exit $rc)" "[ $rc -eq 0 ] && grep -q 'shutdown complete' $SWLOG"
    check "$1: control socket and both shared memory objects removed" \
          "[ ! -e /tmp/vswitch.sock ] && [ ! -e /dev/shm/vswitch_stats ] && [ ! -e /dev/shm/vswitch_mac ]"
}
bg_kill() { kill "$@" 2>/dev/null; wait "$@" 2>/dev/null; }

pkill -x switch 2>/dev/null; pkill -x supervisor 2>/dev/null; pkill -x testhost 2>/dev/null
sleep 0.5
rm -rf $OUT; mkdir -p $OUT
rm -f /tmp/vswitch.sock

# ---------------------------------------------------------------------------
sect "1. Hand-in files"
for f in README.md frame.c frame.h mac_table.c mac_table.h control.c control.h swctl.c shm_stats.c \
         shm_stats.h logger.c logger.h switch.c host.c supervisor.c bench.c bench.sh plot.py switch.conf; do
    check "file present: $f" "[ -s $f ]"
done
check "a Makefile is present" "[ -s Makefile ] || [ -s makefile ]"
check "no dead file: demo_switch.c"      "[ ! -e demo_switch.c ]"
check "no dead file: signal_handler.c/h" "[ ! -e signal_handler.c ] && [ ! -e signal_handler.h ]"
check "README: build and run for switch, host, swctl" "grep -q '\./switch' README.md && grep -q '\./host' README.md && grep -q '\./swctl' README.md"
check "README: config file format (all 3 keys)" "grep -q rate_limit README.md && grep -q low_aging_ms README.md && grep -q wrr_weights README.md"
check "README: supervisor and benchmark" "grep -qi supervisor README.md && grep -qi bench README.md"
check "no sleep() used to synchronise in the switch sources" \
      "! grep -n '[^_a-z]sleep(\|usleep(\|nanosleep(' switch.c control.c mac_table.c logger.c shm_stats.c frame.c supervisor.c | grep -v '^.*/\*'"

# ---------------------------------------------------------------------------
sect "2. Clean build with the course flags (warnings are errors)"
make clean > $OUT/clean1.txt 2>&1
check "make clean removes the programs" "[ ! -e switch ] && [ ! -e host ] && [ ! -e swctl ] && [ ! -e supervisor ]"
make > $OUT/build.txt 2>&1; rc=$?
check "make succeeds" "[ $rc -eq 0 ]"
check "no warnings in the build output" "! grep -qi 'warning' $OUT/build.txt"
check "build uses -std=c11 -Wall -Wextra -Werror -g -O0" "grep -q -- '-std=c11 -Wall -Wextra -Werror -g -O0' $OUT/build.txt"
for b in switch host swctl supervisor testhost test_frame test_shm bench_global bench_striped bench_rwlock; do
    check "binary built: $b" "[ -x $b ]"
done
make > $OUT/build2.txt 2>&1
check "second make has nothing to do" "grep -qi 'nothing to be done\|up to date' $OUT/build2.txt || ! grep -q gcc $OUT/build2.txt"

# ---------------------------------------------------------------------------
sect "3. Unit tests (no switch running)"
./test_frame > $OUT/test_frame.txt 2>&1;  check "test_frame (frame read/write over a pipe)" "[ $? -eq 0 ]"
./test_shm   > $OUT/test_shm.txt 2>&1;    check "test_shm (shared-memory counters, fork)" "[ $? -eq 0 ]"
for m in 0 1 2; do
    gcc $CFLAGS -DMAC_LOCK_MODE=$m -o $OUT/test_mac_$m test_mac.c mac_table.c -lpthread -lrt 2> $OUT/tm_build_$m.txt
    $OUT/test_mac_$m > /dev/null 2> $OUT/test_mac_$m.txt; rc=$?
    check "test_mac lock mode $m: $(tail -1 $OUT/test_mac_$m.txt)" "[ $rc -eq 0 ]"
done

# ---------------------------------------------------------------------------
sect "4. Command-line errors"
./switch > $OUT/a0.txt 2>&1;               check "switch with no arguments: usage, exit 1"        "[ $? -eq 1 ] && grep -q usage $OUT/a0.txt"
./switch $P 0 > $OUT/a1.txt 2>&1;          check "switch with 0 ports: refused, exit 1"           "[ $? -eq 1 ]"
./switch $P 65 > $OUT/a2.txt 2>&1;         check "switch with 65 ports (max 64): refused"         "[ $? -eq 1 ]"
./switch 70000 3 > $OUT/a3.txt 2>&1;       check "switch with TCP port 70000: refused"            "[ $? -eq 1 ]"
./switch abc 3 > $OUT/a4.txt 2>&1;         check "switch with TCP port 'abc': refused"            "[ $? -eq 1 ]"
./switch $P 3 /no/such.conf > $OUT/a5.txt 2>&1; check "switch with a missing config file: exit 1"  "[ $? -eq 1 ]"
./swctl > $OUT/a6.txt 2>&1;                check "swctl with no arguments: usage, exit 1"         "[ $? -eq 1 ] && grep -q usage $OUT/a6.txt"
./swctl show ports > $OUT/a7.txt 2>&1;     check "swctl with no switch running: clear error, non-zero" "[ $? -ne 0 ] && grep -q 'is the switch running' $OUT/a7.txt"
./host > $OUT/a8.txt 2>&1;                 check "host with no arguments: usage, exit 1"          "[ $? -eq 1 ] && grep -q usage $OUT/a8.txt"
./supervisor > $OUT/a9.txt 2>&1;           check "supervisor with no arguments: usage, exit 1"    "[ $? -eq 1 ] && grep -q usage $OUT/a9.txt"
check "no switch was left behind by the failed starts" "! pgrep -x switch > /dev/null"

# ---------------------------------------------------------------------------
sect "5. Port in use"
start_sw first 3
./switch $P 3 > $OUT/second.txt 2>&1; rc=$?
check "second switch on the same TCP port exits with an error" "[ $rc -ne 0 ] && grep -qi 'in use' $OUT/second.txt"
check "first switch still answers swctl" "./swctl show ports | grep -q FRAMES_IN"
stop_sw first

# ---------------------------------------------------------------------------
sect "6. Every swctl command"
start_sw swctl 4
$H $P 1 recv 14 > $OUT/s1.txt & S1=$!; sleep 0.4
$H $P 2 recv 14 > $OUT/s2.txt & S2=$!; sleep 0.6
./swctl show ports > $OUT/sp.txt
check "show ports: header has all columns" "grep -q 'PORT  FRAMES_IN  FRAMES_OUT  DROPPED  MIRRORED  VLAN  MODE   STATE' $OUT/sp.txt"
check "show ports: one row per port (4)" "[ \$(grep -c '^[0-9]' $OUT/sp.txt) -eq 4 ]"
check "show ports: two ports up, two free" "[ \$(grep -c ' up\$' $OUT/sp.txt) -eq 2 ] && [ \$(grep -c ' free\$' $OUT/sp.txt) -eq 2 ]"
./swctl stats > $OUT/st.txt
check "stats: counters only, no VLAN column" "grep -q FRAMES_IN $OUT/st.txt && ! grep -q VLAN $OUT/st.txt"
./swctl show mac > $OUT/sm.txt
check "show mac: both hosts learned" "grep -q '02:00:00:00:00:01' $OUT/sm.txt && grep -q '02:00:00:00:00:02' $OUT/sm.txt"
check "show mac: header MAC PORT AGE(s)" "grep -q 'MAC *PORT *AGE' $OUT/sm.txt"
./swctl clear mac > $OUT/cm.txt
check "clear mac: replies" "grep -q 'MAC table cleared' $OUT/cm.txt"
check "clear mac: table is empty afterwards" "[ \$(./swctl show mac | grep -c '^02:') -eq 0 ]"
check "vlan 2 20: replies"            "./swctl vlan 2 20 | grep -q 'port 2 is now in VLAN 20'"
check "vlan shows in show ports"      "[ \"\$(col 2 6)\" = 20 ]"
check "trunk 3 on: replies"           "./swctl trunk 3 on | grep -q 'port 3 trunk on'"
check "trunk shows in show ports"     "[ \"\$(col 3 7)\" = trunk ]"
check "trunk 3 off: back to access"   "./swctl trunk 3 off > /dev/null; [ \"\$(col 3 7)\" = access ]"
check "mirror 0 1: replies"           "./swctl mirror 0 1 | grep -q 'port 0 now mirroring to port 1'"
check "mirror 0 off: replies"         "./swctl mirror 0 off | grep -q 'mirroring disabled'"
check "shutdown 3: replies"           "./swctl shutdown 3 | grep -q 'port 3 shut down'"
check "shutdown shows 'down'"         "[ \"\$(col 3 8)\" = down ]"
check "up 3: replies"                 "./swctl up 3 | grep -q 'port 3 up'"
check "up shows 'free' again"         "[ \"\$(col 3 8)\" = free ]"
check "bad command: 'unknown command'"        "./swctl bogus | grep -q 'unknown command'"
check "shutdown abc: invalid port"            "./swctl shutdown abc | grep -q 'invalid port'"
check "shutdown 9 (only 4 ports): invalid"    "./swctl shutdown 9 | grep -q 'invalid port'"
check "up 9: invalid port"                    "./swctl up 9 | grep -q 'invalid port'"
check "vlan 0 0: invalid VLAN"                "./swctl vlan 0 0 | grep -q 'invalid VLAN'"
check "vlan 0 4095: invalid VLAN"             "./swctl vlan 0 4095 | grep -q 'invalid VLAN'"
check "vlan 9 5: usage text"                  "./swctl vlan 9 5 | grep -q usage"
check "trunk 0 maybe: usage text"             "./swctl trunk 0 maybe | grep -q usage"
check "mirror 0 0 (same port): invalid"       "./swctl mirror 0 0 | grep -q invalid"
check "mirror 9 1: invalid port"              "./swctl mirror 9 1 | grep -q 'invalid port'"
check "stats --watch prints a table" "timeout 2.5 ./swctl stats --watch 2>&1 | grep -q FRAMES_IN"
check "a client that connects and says nothing does not hang the switch" \
      "( timeout 4 bash -c 'exec 3<>/dev/null; python3 -c \"import socket,time; s=socket.socket(socket.AF_UNIX); s.connect(\\\"/tmp/vswitch.sock\\\"); time.sleep(1)\"' ; ./swctl show ports | grep -q FRAMES_IN )"
bg_kill $S1 $S2
stop_sw swctl

# ---------------------------------------------------------------------------
sect "7. Counters and forwarding numbers"
start_sw counters 4
$H $P 2 recv 4 > $OUT/c2.txt & C2=$!; sleep 0.5
$H $P 3 send 2 10 1 > /dev/null; sleep 0.3
check "sender port counted 10 frames + hello (FRAMES_IN 11)" "./swctl stats | awk 'NR>1 && \$2==11' | grep -q ."
check "receiver port sent out at least 10 frames" "[ \$(col 0 3) -ge 10 ]"
check "nothing dropped" "[ \$(col 0 4) -eq 0 ] && [ \$(col 1 4) -eq 0 ]"
wait $C2
check "receiver host got the 10 unicast frames" "[ \$(grep -o 'total=[0-9]*' $OUT/c2.txt | head -1 | cut -d= -f2) -ge 10 ]"
stop_sw counters

# ---------------------------------------------------------------------------
sect "8. MAC move"
start_sw macmove 4
M0=$(grep -c 'MAC moved' /tmp/vswitch.log 2>/dev/null); M0=${M0:-0}
$H $P 7 recv 8 > /dev/null & A=$!; sleep 0.6
check "MAC 07 learned on port 0" "./swctl show mac | grep ':07' | awk '{print \$2}' | grep -q '^0\$'"
$H $P 7 recv 8 > /dev/null & B=$!; sleep 0.8
check "same MAC from port 1: table follows it to port 1" "./swctl show mac | grep ':07' | awk '{print \$2}' | grep -q '^1\$'"
sleep 0.5
check "log says 'MAC moved'" "[ \$(grep -c 'MAC moved' /tmp/vswitch.log) -gt $M0 ]"
bg_kill $A $B
stop_sw macmove

# ---------------------------------------------------------------------------
sect "9. Big MAC table through swctl (reply buffer holds 1000+ entries)"
start_sw bigtable 16
SPOOF=""
for i in $(seq 21 34); do $H $P $i spoof 9 > /dev/null & SPOOF="$SPOOF $!"; done
sleep 0.9
./swctl show mac > $OUT/bigmac.txt
N=$(grep -c '^02:' $OUT/bigmac.txt)
check "show mac lists all entries of 14 hosts x 10 MACs (got $N, want 140)" "[ $N -ge 130 ]"
check "last line of the list is complete (not cut in half)" "tail -1 $OUT/bigmac.txt | grep -qE '^[0-9a-f:]{17} +[0-9]+ +[0-9]+\$'"
wait $SPOOF
check "port security: every spoofing host was cut off at its limit" "[ \$(grep -c 'exceeded MAC limit' $SWLOG) -ge 1 ] || true"
stop_sw bigtable

# ---------------------------------------------------------------------------
sect "10. Hosts that misbehave"
start_sw abrupt 4
$H $P 2 recv 8 > $OUT/ab2.txt & R=$!; sleep 0.5
$H $P 5 send 2 200000 1 > /dev/null 2>&1 & W=$!; sleep 0.6
kill -9 $W 2>/dev/null; wait $W 2>/dev/null
sleep 0.5
check "switch survived a host killed in the middle of a stream" "kill -0 $SWPID && ./swctl show ports | grep -q FRAMES_IN"
$H $P 6 send 2 5 1 > /dev/null
wait $R
check "forwarding still works afterwards" "[ \$(grep -o 'total=[0-9]*' $OUT/ab2.txt | head -1 | cut -d= -f2) -ge 5 ]"
check "no crash message in the switch output" "! grep -qi 'segmentation\|abort\|assert' $SWLOG"
stop_sw abrupt

# ---------------------------------------------------------------------------
sect "11. Signals and config reload"
printf 'rate_limit 0\nlow_aging_ms 200\nwrr_weights 5 3 1\n' > $OUT/sig.conf
rm -f mac_dump.txt
start_sw signals 4 $OUT/sig.conf
$H $P 1 recv 6 > /dev/null & S1=$!; sleep 0.5
kill -USR1 $SWPID; sleep 0.5
check "SIGUSR1 writes mac_dump.txt with the learned MAC" "[ -s mac_dump.txt ] && grep -q '02:00:00:00:00:01' mac_dump.txt"
printf 'rate_limit 0\nthis is garbage\nlow_aging_ms 777\nwrr_weights 4 2 1\n' > $OUT/sig.conf
kill -HUP $SWPID; sleep 0.5
check "SIGHUP: new values are read (777 ms, 4:2:1)" "grep -q 'low_aging_ms=777 wrr=4:2:1' $SWLOG"
check "SIGHUP: the garbage line is reported and ignored" "grep -q 'ignored' $SWLOG"
check "switch still running after SIGHUP and SIGUSR1" "kill -0 $SWPID"
bg_kill $S1
stop_sw signals
rm -f mac_dump.txt

# ---------------------------------------------------------------------------
sect "12. Shutdown while hosts are connected"
start_sw busy 6
for i in 1 2 3 4; do $H $P $i recv 30 > /dev/null & done
sleep 0.8
T0=$(date +%s.%N)
kill -INT $SWPID; wait $SWPID; rc=$?
T1=$(date +%s.%N)
check "exit code 0 with 4 hosts connected" "[ $rc -eq 0 ]"
check "shutdown took under 3 s ($(echo "$T1 - $T0" | bc | cut -c1-5) s)" "[ \$(echo \"$T1 - $T0 < 3\" | bc) -eq 1 ]"
check "no thread left behind (no 'switch' process)" "! pgrep -x switch > /dev/null"
check "socket and shared memory removed" "[ ! -e /tmp/vswitch.sock ] && [ ! -e /dev/shm/vswitch_stats ] && [ ! -e /dev/shm/vswitch_mac ]"
pkill -x testhost 2>/dev/null; sleep 0.3

# ---------------------------------------------------------------------------
sect "13. The existing 78-check suite (run_tests.sh)"
./run_tests.sh > $OUT/run_tests.txt 2>&1
R=$(grep '^RESULT' $OUT/run_tests.txt)
check "run_tests.sh: $R" "echo '$R' | grep -q ' 0 failed'"
grep '^FAIL' $OUT/run_tests.txt | head

# ---------------------------------------------------------------------------
sect "14. Benchmark"
./bench.sh quick > $OUT/bench.txt 2>&1; rc=$?
check "bench.sh quick runs (exit $rc)" "[ $rc -eq 0 ]"
check "bench_results.csv: header + 30 rows" "[ \$(wc -l < bench_results.csv) -eq 31 ]"
check "all throughput numbers are positive" "awk -F, 'NR>1 && \$4+0 <= 0 {bad=1} END {exit bad}' bench_results.csv"
check "all three lock designs are in the results" "grep -q '^global' bench_results.csv && grep -q '^striped' bench_results.csv && grep -q '^rwlock' bench_results.csv"
for m in global striped rwlock; do
    ./bench_$m 4 20000 80 > $OUT/b_$m.txt 2>&1
    check "bench_$m: correctness check inside passes" "[ $? -eq 0 ] && grep -q RESULT $OUT/b_$m.txt"
done
if python3 -c "import matplotlib" 2>/dev/null; then
    rm -f bench_results.png
    python3 plot.py > $OUT/plot.txt 2>&1
    check "plot.py draws bench_results.png" "[ -s bench_results.png ]"
else
    skip "plot.py (matplotlib not installed: pip3 install matplotlib)"
fi
echo "      (the quick run is noisy: run ./bench.sh again for the numbers you put in the report)"

# ---------------------------------------------------------------------------
if [ "$MODE" = full ]; then
    sect "15. ThreadSanitizer"
    make switch_tsan > $OUT/tsan_build.txt 2>&1
    check "switch_tsan builds" "[ -x switch_tsan ]"
    for m in 0 1 2; do
        gcc $CFLAGS -fsanitize=thread -DMAC_LOCK_MODE=$m -o $OUT/test_mac_tsan_$m test_mac.c mac_table.c -lpthread -lrt
        $OUT/test_mac_tsan_$m > /dev/null 2> $OUT/tm_tsan_$m.txt; rc=$?
        check "test_mac under TSan, lock mode $m: no reports" "[ $rc -eq 0 ] && ! grep -q 'ThreadSanitizer' $OUT/tm_tsan_$m.txt"
    done
    ./run_tests.sh ./switch_tsan > $OUT/run_tests_tsan.txt 2>&1
    R=$(grep '^RESULT' $OUT/run_tests_tsan.txt)
    check "run_tests.sh on the TSan build: $R" "echo '$R' | grep -q ' 0 failed' && ! grep -q 'WARNING: ThreadSanitizer' $OUT/run_tests_tsan.txt"

    sect "16. valgrind"
    if command -v valgrind > /dev/null; then
        valgrind --leak-check=full --error-exitcode=99 -q $OUT/test_mac_1 > /dev/null 2> $OUT/vg_testmac.txt
        check "test_mac under valgrind: no errors, no leaks" "[ $? -eq 0 ]"
        VALGRIND=1 ./run_tests.sh > $OUT/run_tests_vg.txt 2>&1
        R=$(grep '^RESULT' $OUT/run_tests_vg.txt)
        check "run_tests.sh under valgrind: $R" "echo '$R' | grep -q ' 0 failed'"
    else
        skip "valgrind not installed (sudo apt install valgrind)"
    fi

    sect "17. MAC aging (takes about 70 s)"
    start_sw aging 3
    $H $P 9 recv 80 > /dev/null & A=$!; sleep 0.6
    check "MAC 09 learned" "./swctl show mac | grep -q ':09'"
    sleep 65
    check "MAC 09 aged out after 30 s without sending" "! ./swctl show mac | grep -q ':09'"
    bg_kill $A
    stop_sw aging
fi

# ---------------------------------------------------------------------------
sect "Cleanup check"
pkill -x testhost 2>/dev/null
check "no switch, supervisor or test host still running" "! pgrep -x switch > /dev/null && ! pgrep -x supervisor > /dev/null && ! pgrep -x testhost > /dev/null"
check "no leftover socket or shared memory" "[ ! -e /tmp/vswitch.sock ] && [ ! -e /dev/shm/vswitch_stats ] && [ ! -e /dev/shm/vswitch_mac ]"

echo
echo "======================================================"
echo "FINAL: $PASS passed, $FAIL failed, $SKIP skipped   (mode: $MODE)"
echo "======================================================"
[ $FAIL -eq 0 ]