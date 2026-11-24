#define _POSIX_C_SOURCE 200809L

/*
 * Unit tests for mac_table.c (no switch needed).
 *   gcc -std=c11 -Wall -Wextra -Werror -g -O0 [-DMAC_LOCK_MODE=0|1|2] \
 *       -o test_mac test_mac.c mac_table.c -lpthread -lrt
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>

#include "mac_table.h"

void log_push(const char *fmt, ...) { (void)fmt; }

static int passed = 0, failed = 0;
#define CHECK(name, cond) do { \
    if (cond) { passed++; } \
    else { failed++; fprintf(stderr, "FAIL  %s  (line %d)\n", name, __LINE__); } \
} while (0)

static void mk(uint8_t mac[6], int i)
{
    mac[0] = 0x02; mac[1] = 0; mac[2] = 0; mac[3] = 0;
    mac[4] = (uint8_t)(i >> 8); mac[5] = (uint8_t)(i & 0xff);
}

static int count_free(MacTable *t)
{
    int n = 0;
    for (int i = t->s->free_head; i >= 0 && n <= MAC_TABLE_MAX; i = t->s->pool[i].next)
        n++;
    return n;
}

static int snap_count(MacTable *t)
{
    static MacSnap sn[MAC_TABLE_MAX + 8];
    return mac_table_snapshot(t, sn, MAC_TABLE_MAX + 8);
}

/* ---------------- basic behaviour ---------------- */
static void test_basic(void)
{
    MacTable t; uint8_t a[6], b[6], c[6];
    CHECK("init private", mac_table_init(&t, NULL, 0) == 0);
    mk(a, 1); mk(b, 2); mk(c, 3);

    CHECK("unknown MAC -> -1", mac_table_lookup(&t, a) == -1);
    CHECK("learn ok", mac_table_learn(&t, a, 4) == 0);
    CHECK("lookup finds port 4", mac_table_lookup(&t, a) == 4);
    CHECK("one entry", snap_count(&t) == 1);

    CHECK("learn again same port ok", mac_table_learn(&t, a, 4) == 0);
    CHECK("still one entry", snap_count(&t) == 1);

    CHECK("MAC moves to port 7", mac_table_learn(&t, a, 7) == 0);
    CHECK("lookup follows the move", mac_table_lookup(&t, a) == 7);
    CHECK("move does not duplicate", snap_count(&t) == 1);

    mac_table_learn(&t, b, 7);
    mac_table_learn(&t, c, 8);
    CHECK("three entries", snap_count(&t) == 3);

    MacSnap sn[8];
    int n = mac_table_snapshot(&t, sn, 8);
    int found_c = 0;
    for (int i = 0; i < n; i++)
        if (memcmp(sn[i].mac, c, 6) == 0 && sn[i].port_idx == 8 && sn[i].age >= 0 && sn[i].age < 5)
            found_c = 1;
    CHECK("snapshot has mac, port and age", found_c);
    CHECK("snapshot respects max", mac_table_snapshot(&t, sn, 2) == 2);

    mac_table_remove_port(&t, 7);
    CHECK("remove_port drops port 7 entries", mac_table_lookup(&t, a) == -1 && mac_table_lookup(&t, b) == -1);
    CHECK("remove_port keeps other ports", mac_table_lookup(&t, c) == 8);

    mac_table_clear(&t);
    CHECK("clear empties the table", snap_count(&t) == 0 && mac_table_lookup(&t, c) == -1);
    CHECK("free list full after clear", count_free(&t) == MAC_TABLE_MAX);
    mac_table_destroy(&t);
}

/* ---------------- port security ---------------- */
static void test_port_security(void)
{
    MacTable t; uint8_t m[6];
    mac_table_init(&t, NULL, 0);
    int ok = 1;
    for (int i = 0; i < MAC_PORT_MAX_MACS; i++) {
        mk(m, 100 + i);
        if (mac_table_learn(&t, m, 5) != 0) ok = 0;
    }
    CHECK("10 MACs on one port accepted", ok);
    mk(m, 200);
    CHECK("11th MAC on that port rejected (-1)", mac_table_learn(&t, m, 5) == -1);
    CHECK("rejected MAC not stored", mac_table_lookup(&t, m) == -1);
    mk(m, 100);
    CHECK("known MAC on a full port still refreshes", mac_table_learn(&t, m, 5) == 0);
    mk(m, 201);
    CHECK("another port is not affected", mac_table_learn(&t, m, 6) == 0);
    mac_table_destroy(&t);
}

/* ---------------- capacity and LRU ---------------- */
static void test_lru(void)
{
    MacTable t; uint8_t m[6];
    mac_table_init(&t, NULL, 0);

    for (int i = 0; i < MAC_TABLE_MAX; i++) {
        mk(m, i);
        mac_table_learn(&t, m, i / 9);            /* 9 per port: under the limit */
    }
    CHECK("table filled to its maximum", snap_count(&t) == MAC_TABLE_MAX);
    CHECK("free list empty when full", count_free(&t) == 0);

    /* make MAC #500 the oldest entry */
    mk(m, 500);
    for (int i = 0; i < MAC_TABLE_MAX; i++)
        if (t.s->pool[i].used && memcmp(t.s->pool[i].mac, m, 6) == 0)
            t.s->pool[i].last_seen -= 1000;

    uint8_t nm[6]; mk(nm, 5000);
    CHECK("learn into a full table ok", mac_table_learn(&t, nm, 200) == 0);
    CHECK("table stays at the maximum", snap_count(&t) == MAC_TABLE_MAX);
    CHECK("new MAC is in", mac_table_lookup(&t, nm) == 200);
    CHECK("the oldest entry was evicted", mac_table_lookup(&t, m) == -1);
    mk(m, 501);
    CHECK("a younger entry is still there", mac_table_lookup(&t, m) == 501 / 9);

    for (int k = 0; k < 50; k++) {                /* keep pushing: never overflows */
        mk(m, 6000 + k);
        mac_table_learn(&t, m, 300 + k);
    }
    CHECK("50 more learns keep it at the maximum", snap_count(&t) == MAC_TABLE_MAX);
    CHECK("used + free = pool size", snap_count(&t) + count_free(&t) == MAC_TABLE_MAX);
    mac_table_destroy(&t);
}

/* ---------------- shared memory persistence ---------------- */
static void test_persistence(void)
{
    const char *name = "/vswitch_mac_unittest";
    MacTable t1, t2, t3, t4; uint8_t m[6];

    CHECK("init named shm", mac_table_init(&t1, name, 0) == 0);
    for (int i = 1; i <= 5; i++) { mk(m, i); mac_table_learn(&t1, m, i); }

    /* a second handle on the same object = the restarted switch */
    CHECK("reattach with persist=1", mac_table_init(&t2, name, 1) == 0);
    int kept = 1;
    for (int i = 1; i <= 5; i++) { mk(m, i); if (mac_table_lookup(&t2, m) != i) kept = 0; }
    CHECK("reattach keeps all 5 entries", kept && snap_count(&t2) == 5);

    /* simulate a crash in the middle of a list update: scramble every link */
    for (int b = 0; b < MAC_TABLE_BUCKETS; b++) t1.s->head[b] = 123456;
    t1.s->free_head = -77;
    for (int i = 0; i < MAC_TABLE_MAX; i++) t1.s->pool[i].next = 99999;
    CHECK("reattach after scrambled links", mac_table_init(&t3, name, 1) == 0);
    kept = 1;
    for (int i = 1; i <= 5; i++) { mk(m, i); if (mac_table_lookup(&t3, m) != i) kept = 0; }
    CHECK("links rebuilt, 5 entries recovered", kept && snap_count(&t3) == 5);
    CHECK("free list rebuilt", count_free(&t3) == MAC_TABLE_MAX - 5);

    /* a slot that was allocated but never linked is freed on reattach */
    t3.s->pool[900].used = 0;
    CHECK("table usable after rebuild", mac_table_learn(&t3, m, 1) == 0);

    /* persist=0 must start empty even if the object exists */
    CHECK("init persist=0 on existing object", mac_table_init(&t4, name, 0) == 0);
    CHECK("persist=0 starts empty", snap_count(&t4) == 0);

    mac_table_destroy(&t4);
    mac_table_destroy(&t3);
    mac_table_destroy(&t2);
    mac_table_destroy(&t1);

    MacTable t5;
    CHECK("persist=1 with no object creates one", mac_table_init(&t5, name, 1) == 0);
    CHECK("... and it is empty", snap_count(&t5) == 0);
    mac_table_destroy(&t5);
}

/* ---------------- concurrency stress ---------------- */
static MacTable g_t;

static void *stress(void *arg)
{
    uint32_t x = 88172645u + (uint32_t)(uintptr_t)arg * 2654435761u;
    uint8_t  m[6];
    for (int i = 0; i < 40000; i++) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        int id = (int)(x % 1500);
        mk(m, id);
        switch ((x >> 10) % 10) {
        case 0: case 1: case 2: case 3:
            (void)mac_table_learn(&g_t, m, (int)((x >> 16) % 160)); break;
        case 4: case 5: case 6: case 7:
            (void)mac_table_lookup(&g_t, m); break;
        case 8:
            if (((x >> 20) % 50) == 0) mac_table_remove_port(&g_t, (int)((x >> 16) % 160));
            break;
        default: {
            static __thread MacSnap sn[64];
            (void)mac_table_snapshot(&g_t, sn, 64);
        }
        }
    }
    return NULL;
}

static void test_stress(void)
{
    mac_table_init(&g_t, NULL, 0);
    pthread_t th[6];
    for (long i = 0; i < 6; i++) pthread_create(&th[i], NULL, stress, (void *)(i + 1));
    for (int i = 0; i < 6; i++) pthread_join(th[i], NULL);

    int used = snap_count(&g_t);
    CHECK("stress: never above the maximum", used <= MAC_TABLE_MAX);
    CHECK("stress: used + free = pool size", used + count_free(&g_t) == MAC_TABLE_MAX);

    static MacSnap all[MAC_TABLE_MAX + 8];
    int n = mac_table_snapshot(&g_t, all, MAC_TABLE_MAX + 8);
    int dup = 0;
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (memcmp(all[i].mac, all[j].mac, 6) == 0) dup++;
    CHECK("stress: no MAC stored twice", dup == 0);
    mac_table_clear(&g_t);
    CHECK("stress: clear leaves a full free list", count_free(&g_t) == MAC_TABLE_MAX);
    mac_table_destroy(&g_t);
}

/* ---------------- aging thread starts and stops cleanly ---------------- */
static void test_aging_thread(void)
{
    MacTable t;
    mac_table_init(&t, NULL, 0);
    CHECK("aging thread starts", mac_table_start_aging(&t) == 0);
    mac_table_destroy(&t);                         /* must return at once, not after 30 s */
    CHECK("aging thread stops promptly", 1);
}

int main(void)
{
    /* the table prints a line for every rejected MAC: keep the screen clean */
    if (freopen("/dev/null", "w", stdout) == NULL) return 2;
    test_basic();
    test_port_security();
    test_lru();
    test_persistence();
    test_stress();
    test_aging_thread();
    fprintf(stderr, "test_mac (lock mode %d): %d passed, %d failed\n", MAC_LOCK_MODE, passed, failed);
    return failed ? 1 : 0;
}


