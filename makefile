CC      = gcc
CFLAGS  = -std=c11 -Wall -Wextra -Werror -g -O0
LDLIBS  = -lpthread -lrt

PROGS = switch host swctl test_frame test_shm testhost supervisor \
        bench_global bench_striped bench_rwlock
SWITCH_SRCS = switch.c frame.c mac_table.c shm_stats.c control.c logger.c

all: $(PROGS)

switch: $(SWITCH_SRCS) frame.h mac_table.h shm_stats.h control.h logger.h
	$(CC) $(CFLAGS) -o $@ $(SWITCH_SRCS) $(LDLIBS)

host: host.c frame.c frame.h
	$(CC) $(CFLAGS) -o $@ host.c frame.c $(LDLIBS)

swctl: swctl.c shm_stats.c shm_stats.h control.h
	$(CC) $(CFLAGS) -o $@ swctl.c shm_stats.c $(LDLIBS)

test_frame: test_frame.c frame.c frame.h
	$(CC) $(CFLAGS) -o $@ test_frame.c frame.c $(LDLIBS)

test_shm: test_shm.c shm_stats.c shm_stats.h
	$(CC) $(CFLAGS) -o $@ test_shm.c shm_stats.c $(LDLIBS)

# separate ThreadSanitizer build of the switch
switch_tsan: $(SWITCH_SRCS)
	$(CC) $(CFLAGS) -fsanitize=thread -o $@ $(SWITCH_SRCS) $(LDLIBS)

tsan: switch_tsan

clean:
	rm -f $(PROGS) switch_tsan bench_tsan_*

supervisor: supervisor.c
	$(CC) $(CFLAGS) -o $@ supervisor.c $(LDLIBS)

# the benchmark: the same mac_table.c built with three lock modes
bench_global: bench.c mac_table.c mac_table.h
	$(CC) $(CFLAGS) -DMAC_LOCK_MODE=0 -o $@ bench.c mac_table.c $(LDLIBS)

bench_striped: bench.c mac_table.c mac_table.h
	$(CC) $(CFLAGS) -DMAC_LOCK_MODE=1 -o $@ bench.c mac_table.c $(LDLIBS)

bench_rwlock: bench.c mac_table.c mac_table.h
	$(CC) $(CFLAGS) -DMAC_LOCK_MODE=2 -o $@ bench.c mac_table.c $(LDLIBS)

.PHONY: all clean tsan

testhost: testhost.c frame.c frame.h
	$(CC) $(CFLAGS) -o $@ testhost.c frame.c $(LDLIBS)