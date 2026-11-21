# Notes for the report

## Threads

| Thread | Count | Job |
|---|---|---|
| main | 1 | accept loop, port semaphore (sem_timedwait), reaps receivers, runs shutdown |
| receiver | 1 per port | frame_read, learn MAC, look up, enqueue (never blocks) |
| sender | 1 per port | pq_pop (WRR + aging), tb_consume (token), frame_write |
| signal | 1 | sigwait: SIGINT/SIGTERM shutdown, SIGHUP reload config, SIGUSR1 dump MAC table |
| token timer | 1 | every 100 ms adds rate/10 tokens to each UP port |
| MAC aging | 1 | every T s removes entries older than T |
| control | 1 | UNIX socket for swctl, poll() on socket + wake-up pipe |
| logger | 1 | drains the log queue into /tmp/vswitch.log with timestamps |

## Locks and what they protect

| Lock | Type | Protects |
|---|---|---|
| ports_lock | rwlock | fd, state, mirror_to of every port. Forwarding reads, add/remove/mirror write |
| pq.lock (per port) | mutex + cond not_empty | the 3 priority FIFOs, WRR credits, closing, dropped/aged counters |
| tb.lock (per port) | mutex + cond has_token | tokens, carry, closing |
| sf->reflock (per frame) | mutex | refcount of one shared frame |
| bucket lock (64) | mutex | one MAC hash chain (striped locking) |
| count_lock | mutex | MAC total_entries |
| aging_lock | mutex + cond | aging_stop flag (wakes aging thread at shutdown) |
| stats->lock | process-shared mutex | per-port counters in shared memory (switch + swctl) |
| timer_lock | mutex + cond | timer_stop flag |
| log_lock | mutex + cond | log queue |
| g_shutdown, config values | C11 atomics | no lock needed |

## Lock order (never take an earlier lock while holding a later one)

1. ports_lock
2. pq.lock / tb.lock of a port (never two ports' queue locks at once)
3. frame reflock
4. MAC bucket locks, lowest bucket first, one at a time; count_lock after a bucket lock
5. stats->lock
6. log_lock (leaf: nothing is taken while holding it)

## Real bugs found

1. **SIGINT ignored when started from a script.** `./switch &` from a shell
   script never shut down on `kill -INT`. gdb (`thread apply all bt`) showed the
   signal thread still in sigwait. A non-interactive shell starts background
   jobs with SIGINT set to SIG_IGN; the switch inherited it and Linux discards
   an ignored signal even while it is blocked for sigwait. Fix: `signal(sig,
   SIG_DFL)` for each signal before `pthread_sigmask`.
2. **LRU eviction used stale pointers.** evict_lru scanned all buckets, unlocked,
   then unlinked the entry using the `prev` pointer it saved. Another thread
   (aging, another eviction) could change the chain in between: use-after-free.
   Fix: remember the MAC, relock the bucket, search again, remove only if
   still the same entry.
3. **10 ThreadSanitizer reports on shutdown** (original code): g_shutdown was a
   plain volatile int; port_remove destroyed and re-initialised queue mutexes
   while the signal thread was locking them; handler threads that exited on
   their own were never joined (thread leak); the control thread was detached
   and its fd closed by two threads. Fix: atomics, ports keep their mutexes for
   life and are reset under their lock, PORT_CLOSING state, main reaps
   receivers, control thread joined via a self-pipe.
4. **Aging starved HIGH.** With a 100 ms threshold and a backlog, every frame
   was "old", so aging picked low/normal forever. Fix: aging only for LOW, and
   never two aged picks in a row.

## Tests

`./run_tests.sh` (42 checks), also `./run_tests.sh ./switch_tsan` and
`VALGRIND=1 ./run_tests.sh`. All pass, TSan reports 0, valgrind 0 errors.
