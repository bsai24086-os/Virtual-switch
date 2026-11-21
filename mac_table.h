#ifndef MAC_TABLE_H
#define MAC_TABLE_H

#include <stdint.h>
#include <time.h>
#include <pthread.h>

#define MAC_TABLE_BUCKETS  64     /* number of hash buckets              */
#define MAC_TABLE_MAX      1024   /* max total entries (LRU eviction)    */
#define MAC_PORT_MAX_MACS  10     /* max MACs per port (port security)   */
#define MAC_AGING_SECS     30     /* remove entries older than this      */
#define MAC_SHM_NAME       "/vswitch_mac"

/*
 * Lock mode, chosen at compile time (the benchmark builds all three):
 *   0 = one global mutex
 *   1 = striped: one mutex per bucket (default, what the switch uses)
 *   2 = one global read-write lock
 */
#ifndef MAC_LOCK_MODE
#define MAC_LOCK_MODE 1
#endif

/* One entry. Lives in a fixed pool in shared memory; links are indexes. */
typedef struct {
    uint8_t  mac[6];
    int      port_idx;
    time_t   last_seen;
    int      next;        /* next entry in the bucket or the free list, -1 = end */
    int      used;        /* 1 = linked into a bucket */
} MacSlot;

/* Everything in here lives in shared memory and survives a crash. */
typedef struct {
    uint32_t         magic;
    int              head[MAC_TABLE_BUCKETS];
    pthread_mutex_t  bucket_lock[MAC_TABLE_BUCKETS];
    pthread_mutex_t  glock;            /* mode 0 */
    pthread_rwlock_t rwlock;           /* mode 2 */
    pthread_mutex_t  alloc_lock;       /* free list */
    int              free_head;
    MacSlot          pool[MAC_TABLE_MAX];
} MacShm;

/* Process-local handle. */
typedef struct {
    MacShm         *s;
    char            shm_name[64];      /* "" = private anonymous memory */
    pthread_mutex_t aging_lock;
    pthread_cond_t  aging_cond;
    int             aging_stop;
    int             aging_running;
    pthread_t       aging_tid;
} MacTable;

typedef struct {
    uint8_t mac[6];
    int     port_idx;
    long    age;
} MacSnap;

/*
 * shm_name NULL: private memory (benchmark). Otherwise a POSIX shm object.
 * persist 1: attach to an existing object and keep its entries (restart
 * after a crash). persist 0: start empty.
 * Returns 0 or -1.
 */
int   mac_table_init(MacTable *t, const char *shm_name, int persist);
int   mac_table_start_aging(MacTable *t);
void  mac_table_destroy(MacTable *t);   /* joins aging, unlinks the shm object */
int   mac_table_lookup(MacTable *t, const uint8_t mac[6]);
int   mac_table_learn(MacTable *t, const uint8_t mac[6], int port_idx);
void  mac_table_remove_port(MacTable *t, int port_idx);
void  mac_table_clear(MacTable *t);
int   mac_table_snapshot(MacTable *t, MacSnap *out, int max);
void *mac_table_aging_thread(void *arg);

#endif /* MAC_TABLE_H */