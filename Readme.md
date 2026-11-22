# Virtual Switch

A multithreaded Layer 2 switch in C. Hosts connect over TCP, each TCP connection is one switch port.
Needs Linux (tested on WSL), gcc and pthreads.

## Build

```
make            # builds switch, host, swctl, test_frame, test_shm, testhost
make tsan       # ThreadSanitizer build (switch_tsan) for race checking
make clean
```

Flags: `gcc -std=c11 -Wall -Wextra -Werror -g -O0`, linked with `-lpthread -lrt`.

## Run

### switch

```
./switch <tcp_port> <num_ports> [config_file]
```

- `tcp_port`: 1 to 65535, where hosts connect.
- `num_ports`: number of ports the switch accepts at once. A host that connects when all ports are taken gets a "switch full" reply.
- `config_file`: optional, see below.

Example: `./switch 9000 4 switch.conf`

Stop with Ctrl+C or `kill -TERM <pid>`. Both shut down cleanly.

### host

```
./host <switch_ip> <tcp_port> <mac_last_byte> [loop]
```

Connects to the switch as a host whose MAC is `00:00:00:00:00:<mac_last_byte>`. Without `loop` it sends 3 frames and exits. With `loop` it keeps sending until Ctrl+C.

Example: `./host 127.0.0.1 9000 1`

### swctl

Control tool for a running switch. It talks to the switch over the UNIX socket `/tmp/vswitch.sock` and reads counters from shared memory.

```
./swctl show ports
./swctl show mac
./swctl clear mac
./swctl stats
./swctl stats --watch
./swctl mirror <src_port> <sniffer_port|off>
./swctl shutdown <port>
./swctl up <port>
./swctl vlan <port> <vlan>
./swctl trunk <port> <on|off>
```

| Command | Effect |
| --- | --- |
| `show ports` | State, VLAN and trunk flag of each port |
| `show mac` | The MAC table |
| `clear mac` | Empties the MAC table |
| `stats`, `stats --watch` | Per-port counters, once or refreshed |
| `mirror` | Copies traffic of one port to a sniffer port |
| `shutdown`, `up` | Takes a port down or brings it back, live |
| `vlan` | Puts an access port into a VLAN |
| `trunk` | Turns trunk mode on or off. A trunk port carries all VLANs |

### testhost

Scripted test host used by `run_tests.sh`. Modes: `send`, `mix`, `recv`, `stall`, `hello`, `full`, `spoof`. See the top of `testhost.c` for the arguments of each mode.

## Config file

One setting per line. `#` starts a comment. Blank lines are ignored.

```
rate_limit    0        # max frames per second each port may send, 0 = no limit
low_aging_ms  200      # a LOW frame older than this many ms is sent next
wrr_weights   5 3 1    # weights for high, normal, low queues
```

Defaults when a line is missing: no rate limit, 200 ms aging, weights 5 3 1.
Reload without restarting: `kill -HUP <switch_pid>`.

## Signals

| Signal | Action |
| --- | --- |
| SIGINT, SIGTERM | Clean shutdown |
| SIGHUP | Reload the config file |
| SIGUSR1 | Write the MAC table to `mac_dump.txt` in the current directory |

## Files the switch creates

| Path | Content |
| --- | --- |
| `/tmp/vswitch.log` | Log file, written by the logger thread |
| `/tmp/vswitch.sock` | Control socket for swctl |
| `/dev/shm/vswitch_stats` | Shared-memory counters |
| `/dev/shm/vswitch_mac` | Shared-memory MAC table (kept across a crash under the supervisor) |
| `mac_dump.txt` | MAC table dump written on SIGUSR1, in the current directory |

## Supervisor

```
./supervisor <tcp_port> <num_ports> [config_file]
```

Runs `./switch` with the same arguments and starts it again if it crashes.
The switch is started with `VSWITCH_PERSIST=1`, so the new process attaches
to the old shared-memory MAC table and keeps what it had learned. A clean
exit (exit code 0, for example after Ctrl+C) ends the supervisor too. More
than 5 crashes within 10 seconds also stops it.

Try it: start `./supervisor 9000 4`, connect a host, run `pkill -9 -x switch`,
then `./swctl show mac`. The table is still filled.

## Benchmark

The MAC table can be built with three lock designs (`MAC_LOCK_MODE` 0 = one
global mutex, 1 = striped locks, 2 = read-write lock).

```
./bench.sh quick     # short run, checks that everything works
./bench.sh           # full run, about 2 minutes, writes bench_results.csv
python3 plot.py      # draws bench_results.png (needs matplotlib)
```

## Tests

```
./final_test.sh                # everything below plus build, CLI and swctl checks (about 4 min)
./final_test.sh full           # also TSan, valgrind and MAC aging (about 15 min)
./run_tests.sh                 # 78 checks on the normal build
./run_tests.sh ./switch_tsan   # same checks under ThreadSanitizer
VALGRIND=1 ./run_tests.sh      # same checks under valgrind
make test_frame test_shm && ./test_frame && ./test_shm   # unit tests
gcc -std=c11 -Wall -Wextra -Werror -g -O0 -DMAC_LOCK_MODE=1 -o test_mac test_mac.c mac_table.c -lpthread -lrt && ./test_mac
```

Run the tests with no other switch running.

## Known limits

- Port security can briefly exceed its limit by one MAC.
- The MAC table is keyed by MAC only, not by MAC and VLAN.
- Tier 3 is not implemented.