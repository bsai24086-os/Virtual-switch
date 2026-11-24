Here is the pattern. It is the same every time, so you can follow it without help. Save it somewhere you can open during the evaluation, such as your phone notes or a text file.

A. In WSL, start the switch

cd /mnt/c/OS/Project01/switch
make
./switch 9000 8

The 9000 is the TCP port and 8 is the number of ports. Pick any port, but use the same number in the forwarding step.

B. In Windows, forward the port (admin PowerShell)

Press the Windows key, type PowerShell, right-click it, choose Run as administrator, click Yes. Then paste:

$wslip = (wsl hostname -I).Trim().Split(' ')[0]
netsh interface portproxy delete v4tov4 listenport=9000 listenaddress=0.0.0.0
netsh interface portproxy add v4tov4 listenport=9000 listenaddress=0.0.0.0 connectport=9000 connectaddress=$wslip
netsh interface portproxy show all

The last line should show 0.0.0.0 9000 172.x.x.x 9000. Redo this block every time you restart WSL or the PC, because the WSL address changes. If you use a new port, also run:

New-NetFirewallRule -DisplayName "vswitch NEWPORT" -Direction Inbound -Protocol TCP -LocalPort NEWPORT -Action Allow

Port 9000 is already allowed, so using 9000 saves a step.

C. Find the address to tell the TA

ipconfig

Use the IPv4 Address under "Wireless LAN adapter Wi-Fi". Today it is 192.168.10.6, and it will be different on the evaluation network. Tell the TA that address and your port.

D. Check it yourself

In PowerShell: Test-NetConnection <your_ip> -Port 9000. You want TcpTestSucceeded : True.
On a phone on the same Wi-Fi: open http://<your_ip>:9000. It will fail with a blank error, but your switch window should log a connection. That is a pass.

E. What the TA's laptop runs

The TA's laptop runs your ./host, or their own test hosts:

./host <your_ip> 9000 02:00:00:00:00:0A

Your switch window prints [port N] host connected, which is what the rules ask for.

F. Local test without any network

In a second WSL terminal:

./host 127.0.0.1 9000 02:00:00:00:00:0A

If something fails

Problem	Check
Test says False or times out	Is the switch running? ss -ltn | grep 9000 in WSL
Worked before, fails now	The WSL address changed, so repeat step B
Works for you, not the TA	Different networks, or Wi-Fi blocks devices from each other. Try a phone hotspot
Not elevated errors	The PowerShell is not "Administrator:"

If the evaluation machine is a plain Linux computer, skip B entirely. Use ip -4 addr or hostname -I to get the address and give that to the TA.


/*    int headdrop = 0;
    if (!pq->closing && q->count >= QUEUE_SIZE) {
        /* head drop: throw away the OLDEST frame to make room */
        shared_frame_unref(sq_pop(q));
        pq->dropped++;
        headdrop = 1;
    }
    if (pq->closing) {
        pq->dropped++;
    } else {
        q->slots[q->tail].sf = sf;
        clock_gettime(CLOCK_MONOTONIC, &q->slots[q->tail].enq);
        q->tail = (q->tail + 1) % QUEUE_SIZE;
        q->count++;
        ok = headdrop ? 2 : 1;
        pthread_cond_signal(&pq->not_empty);
    }

*/


/*
static int enqueue_to(int i, SharedFrame *sf)
{
    shared_frame_ref(sf);
    int rc = pq_push(&ports[i].pq, sf);
    if (rc == 0) {
        shared_frame_unref(sf);
        shm_inc_dropped(g_stats, i);
        if (g_verbose) log_push("port %d: port closing, frame dropped", i);
        return 0;
    }
    if (rc == 2) {
        shm_inc_dropped(g_stats, i);
        if (g_verbose) log_push("port %d: queue full, oldest frame dropped", i);
    }
    return 1;
}

*/




0. Start clean
cd /mnt/c/OS/Project_OS/switch
pkill -x switch; pkill -x twoswitch; pkill -x supervisor; pkill -x testhost
rm -f /tmp/vswitch.sock* /dev/shm/vswitch_*
make
1. Automatic tests
Command	Checks	Time
./run_tests.sh	78 single-switch tests	~3 min
./t3_test.sh	24 two-switch tests	~40 s
./final_test.sh	build, README, swctl, plus run_tests	~4 min
./final_test.sh full	adds TSan, valgrind, aging	~15 min
./run_tests.sh ./switch_tsan	ThreadSanitizer (run make tsan first)	~4 min
2. Basic forwarding (the TA will start here)
T1: ./switch 9000 4
T2: ./testhost 9000 11 recv 30          # host 0B, waits for frames
T3: ./testhost 9000 10 send 11 20 1     # host 0A sends 20 frames to 0B
T3: ./testhost 9000 10 send bc 20 1     # broadcast
T3: ./host 127.0.0.1 9000 1             # your own host: 3 frames
T3: ./host 127.0.0.1 9000 1 loop        # 1 frame/s until Ctrl+C

T1 prints the port number of each new host. testhost MACs are 02:00:00:00:00:XX, and the numbers are decimal: 10=0A, 11=0B, 12=0C.

3. swctl (switch from section 2 still running)
./swctl show ports
./swctl show mac
./swctl stats
./swctl stats --watch        # Ctrl+C to stop
./swctl clear mac
./swctl vlan 1 2             # port 1 into VLAN 2
./swctl trunk 1 on
./swctl mirror 0 2           # copy port 0 traffic to port 2
./swctl mirror 0 off
./swctl shutdown 1
./swctl up 1
4. Slow host (drops are counted, other hosts unaffected)
T1: ./switch 9000 4
T2: ./testhost 9000 9 stall 10          # never reads
T3: ./testhost 9000 11 recv 10
T4: ./testhost 9000 10 send bc 3000 1 200
T4: ./swctl show ports                  # DROPPED > 0 only on the stalled port
5. Signals
T1: ./switch 9000 4 switch.conf
T2: kill -USR1 $(pgrep -x switch); cat mac_dump.txt
T2: kill -HUP $(pgrep -x switch)        # reloads switch.conf
T2: kill -INT $(pgrep -x switch)        # clean shutdown, same as Ctrl+C
6. Two switches (Tier 3 trunk)
T1: ./twoswitch 9000 9001 4
T2: ./testhost 9001 11 recv 30          # host 0B on switch B
T3: ./testhost 9000 10 send 11 100 1 300
T3: VSWITCH_NAME=a ./swctl show mac     # 0b on port 0 = trunk
T3: VSWITCH_NAME=b ./swctl show mac     # 0a on port 0
T3: VSWITCH_NAME=a ./swctl show ports   # port 0 says trunk
T3: pkill -9 -f "switch 9001"           # kill B: A drops port 0 and keeps working
T1: Ctrl+C                              # stops both
7. Supervisor (Tier 3)
T1: ./supervisor 9000 4
T2: ./testhost 9000 11 recv 60
T3: ./testhost 9000 10 send 11 5 1
T3: pkill -9 -x switch                  # crash it
T3: ./swctl show mac                    # MACs still there, switch restarted
T1: Ctrl+C
8. Benchmark (Tier 3)
./bench.sh quick
9. TA laptop over Wi-Fi

In WSL, get the WSL IP:

hostname -I

In PowerShell as Administrator:

netsh interface portproxy add v4tov4 listenport=9000 listenaddress=0.0.0.0 connectport=9000 connectaddress=<WSL_IP>
New-NetFirewallRule -DisplayName "vswitch 9000" -Direction Inbound -Protocol TCP -LocalPort 9000 -Action Allow
ipconfig                                # give the TA the Wi-Fi IPv4 and port 9000

The WSL IP changes after a restart. If the laptop can't connect, run netsh interface portproxy reset and add the rule again with the new IP.

10. After your 20-minute change
make
./run_tests.sh
./t3_test.sh
