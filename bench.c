#define _POSIX_C_SOURCE 200809L

/*
 * MAC table benchmark: a multithreaded load generator.
 *
 *   ./bench_<mode> <threads> <ops_per_thread> <read_percent>
 *
 * Built three times from the same mac_table.c with -DMAC_LOCK_MODE=0|1|2
 * (global mutex, striped locks, read-write lock). Every thread does a fixed
 * number of operations on 512 known MACs: lookups with probability
 * read_percent, otherwise learns (refreshing an entry that already exists).
 * No sleeping: all threads wait on a barrier, the clock runs from the
 * barrier to the last join.
 *
 * Prints one line:  RESULT,<mode>,<threads>,<read_percent>,<Mops/s>
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#include "mac_table.h"

#define NUM_MACS 512          /* 64 ports x 8 MACs, under the 10-per-port limit */

#if MAC_LOCK_MODE == 0
#define MODE_NAME "global"
#elif MAC_LOCK_MODE == 1
#define MODE_NAME "striped"
#else
#define MODE_NAME "rwlock"
#endif

/* the table logs through the logger; the benchmark just drops it */
void log_push(const char *fmt, ...) { (void)fmt; }

static MacTable         g_table;
static pthread_barrier_t g_barrier;
static long             g_ops;
static int              g_read_pct;

static void make_mac(int idx, uint8_t mac[6])
{
    mac[0] = 0x02; mac[1] = 0; mac[2] = 0; mac[3] = 0;
    mac[4] = (uint8_t)(idx >> 8);
    mac[5] = (uint8_t)(idx & 0xff);
}

static void *worker(void *arg)
{
    uint32_t x = 2463534242u + (uint32_t)(uintptr_t)arg * 7919u;
    uint8_t  mac[6];

    pthread_barrier_wait(&g_barrier);
    for (long i = 0; i < g_ops; i++) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;      /* xorshift32 */
        int idx = (int)(x % NUM_MACS);
        make_mac(idx, mac);
        if ((int)((x >> 12) % 100) < g_read_pct)
            (void)mac_table_lookup(&g_table, mac);
        else
            (void)mac_table_learn(&g_table, mac, idx / 8);
    }
    return NULL;
}

int main(int argc, char *argv[])
{
    if (argc != 4) {
        fprintf(stderr, "usage: %s <threads> <ops_per_thread> <read_percent>\n", argv[0]);
        return 1;
    }
    int threads = atoi(argv[1]);
    g_ops       = atol(argv[2]);
    g_read_pct  = atoi(argv[3]);
    if (threads < 1 || threads > 256 || g_ops < 1 || g_read_pct < 0 || g_read_pct > 100) {
        fprintf(stderr, "bad arguments\n");
        return 1;
    }

    if (mac_table_init(&g_table, NULL, 0) != 0) return 1;

    uint8_t mac[6];
    for (int i = 0; i < NUM_MACS; i++) {
        make_mac(i, mac);
        if (mac_table_learn(&g_table, mac, i / 8) != 0) {
            fprintf(stderr, "prefill failed\n");
            return 1;
        }
    }

    pthread_t tid[256];
    if (pthread_barrier_init(&g_barrier, NULL, (unsigned)threads + 1) != 0) {
        perror("pthread_barrier_init");
        return 1;
    }
    for (long i = 0; i < threads; i++) {
        if (pthread_create(&tid[i], NULL, worker, (void *)(i + 1)) != 0) {
            perror("pthread_create");
            return 1;
        }
    }

    struct timespec t0, t1;
    pthread_barrier_wait(&g_barrier);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < threads; i++)
        pthread_join(tid[i], NULL);
    clock_gettime(CLOCK_MONOTONIC, &t1);

    /* correctness check: every MAC must still map to its port */
    int bad = 0;
    for (int i = 0; i < NUM_MACS; i++) {
        make_mac(i, mac);
        if (mac_table_lookup(&g_table, mac) != i / 8) bad++;
    }

    double secs = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
    double mops = (double)threads * (double)g_ops / secs / 1e6;
    printf("RESULT,%s,%d,%d,%.3f\n", MODE_NAME, threads, g_read_pct, mops);

    pthread_barrier_destroy(&g_barrier);
    mac_table_destroy(&g_table);
    if (bad) {
        fprintf(stderr, "FAIL: %d MACs map to the wrong port\n", bad);
        return 1;
    }
    return 0;
}