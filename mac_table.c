#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE  


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "mac_table.h"
#include "logger.h"

#define MAC_MAGIC 0x4d414331u   /* "MAC1" */

/* ------------------------------------------------------------------ */
/*  Hash                                                                */
/* ------------------------------------------------------------------ */

static int mac_hash(const uint8_t mac[6])
{
    uint32_t h = 0;
    for (int i = 0; i < 6; i++)
        h = h * 31 + mac[i];
    return (int)(h % MAC_TABLE_BUCKETS);
}

/* ------------------------------------------------------------------ */
/*  Bucket locking. Which lock is used depends on MAC_LOCK_MODE.        */
/*  Callers never hold two of these at once, so no mode can deadlock.   */
/*  wr = 1 when the caller changes the bucket, 0 when it only reads.    */
/* ------------------------------------------------------------------ */

static void bl(MacTable *t, int b, int wr)
{
#if MAC_LOCK_MODE == 0
    (void)b; (void)wr;
    pthread_mutex_lock(&t->s->glock);
#elif MAC_LOCK_MODE == 1
    (void)wr;
    pthread_mutex_lock(&t->s->bucket_lock[b]);
#else
    (void)b;
    if (wr) pthread_rwlock_wrlock(&t->s->rwlock);
    else    pthread_rwlock_rdlock(&t->s->rwlock);
#endif
}

static void bu(MacTable *t, int b)
{
#if MAC_LOCK_MODE == 0
    (void)b;
    pthread_mutex_unlock(&t->s->glock);
#elif MAC_LOCK_MODE == 1
    pthread_mutex_unlock(&t->s->bucket_lock[b]);
#else
    (void)b;
    pthread_rwlock_unlock(&t->s->rwlock);
#endif
}

/* ------------------------------------------------------------------ */
/*  Free list (alloc_lock). Taken after a bucket lock, never before.    */
/* ------------------------------------------------------------------ */

static int slot_alloc(MacTable *t)
{
    MacShm *s = t->s;
    pthread_mutex_lock(&s->alloc_lock);
    int i = s->free_head;
    if (i >= 0) s->free_head = s->pool[i].next;
    pthread_mutex_unlock(&s->alloc_lock);
    return i;
}

static void slot_free(MacTable *t, int i)
{
    MacShm *s = t->s;
    __atomic_store_n(&s->pool[i].used, 0, __ATOMIC_RELEASE);
    pthread_mutex_lock(&s->alloc_lock);
    s->pool[i].next = s->free_head;
    s->free_head    = i;
    pthread_mutex_unlock(&s->alloc_lock);
}

/* ------------------------------------------------------------------ */
/*  Shared memory setup                                                 */
/* ------------------------------------------------------------------ */

static void init_locks(MacShm *s)
{
    pthread_mutexattr_t ma;
    pthread_rwlockattr_t ra;
    pthread_mutexattr_init(&ma);
    pthread_mutexattr_setpshared(&ma, PTHREAD_PROCESS_SHARED);
    pthread_rwlockattr_init(&ra);
    pthread_rwlockattr_setpshared(&ra, PTHREAD_PROCESS_SHARED);
    for (int i = 0; i < MAC_TABLE_BUCKETS; i++)
        pthread_mutex_init(&s->bucket_lock[i], &ma);
    pthread_mutex_init(&s->glock, &ma);
    pthread_mutex_init(&s->alloc_lock, &ma);
    pthread_rwlock_init(&s->rwlock, &ra);
    pthread_mutexattr_destroy(&ma);
    pthread_rwlockattr_destroy(&ra);
}

/*
 * Rebuild every link from the "used" flags. After a crash the old process
 * may have died in the middle of a list update, so no pointer is trusted.
 * The locks are re-initialised too: the dead process may have held one.
 * Only called when no other switch is running (the supervisor guarantees it).
 */
static int rebuild(MacShm *s)
{
    init_locks(s);
    for (int b = 0; b < MAC_TABLE_BUCKETS; b++) s->head[b] = -1;
    s->free_head = -1;
    int kept = 0;
    for (int i = MAC_TABLE_MAX - 1; i >= 0; i--) {
        MacSlot *e = &s->pool[i];
        if (e->used == 1 && e->port_idx >= 0) {
            int b = mac_hash(e->mac);
            e->next    = s->head[b];
            s->head[b] = i;
            kept++;
        } else {
            e->used = 0;
            e->next = s->free_head;
            s->free_head = i;
        }
    }
    return kept;
}

int mac_table_init(MacTable *t, const char *shm_name, int persist)
{
    memset(t, 0, sizeof(*t));
    int attach = 0;

    if (shm_name == NULL) {
        t->s = mmap(NULL, sizeof(MacShm), PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        if (t->s == MAP_FAILED) { perror("mmap mac table"); return -1; }
    } else {
        snprintf(t->shm_name, sizeof(t->shm_name), "%s", shm_name);
        int fd = -1;
        if (persist) {
            fd = shm_open(t->shm_name, O_RDWR, 0600);
            if (fd >= 0) {
                struct stat st;
                if (fstat(fd, &st) == 0 && (size_t)st.st_size == sizeof(MacShm))
                    attach = 1;
                else {
                    close(fd);
                    fd = -1;
                }
            }
        }
        if (fd < 0) {
            shm_unlink(t->shm_name);
            fd = shm_open(t->shm_name, O_CREAT | O_RDWR, 0600);
            if (fd < 0) { perror("shm_open mac table"); return -1; }
            if (ftruncate(fd, sizeof(MacShm)) < 0) {
                perror("ftruncate mac table");
                close(fd); shm_unlink(t->shm_name); return -1;
            }
        }
        t->s = mmap(NULL, sizeof(MacShm), PROT_READ | PROT_WRITE,
                    MAP_SHARED, fd, 0);
        close(fd);
        if (t->s == MAP_FAILED) {
            perror("mmap mac table");
            shm_unlink(t->shm_name);
            return -1;
        }
        if (attach && t->s->magic != MAC_MAGIC) attach = 0;
    }

    if (attach) {
        int kept = rebuild(t->s);
        printf("[mac] reattached %s, kept %d entries\n", t->shm_name, kept);
    } else {
        memset(t->s, 0, sizeof(MacShm));
        t->s->magic = MAC_MAGIC;
        rebuild(t->s);
    }

    pthread_mutex_init(&t->aging_lock, NULL);
    pthread_cond_init(&t->aging_cond, NULL);
    t->aging_stop = 0;
    return 0;
}

int mac_table_start_aging(MacTable *t)
{
    if (pthread_create(&t->aging_tid, NULL, mac_table_aging_thread, t) != 0) {
        perror("pthread_create aging");
        return -1;
    }
    t->aging_running = 1;
    return 0;
}

void mac_table_destroy(MacTable *t)
{
    if (t->aging_running) {
        pthread_mutex_lock(&t->aging_lock);
        t->aging_stop = 1;
        pthread_cond_signal(&t->aging_cond);
        pthread_mutex_unlock(&t->aging_lock);
        pthread_join(t->aging_tid, NULL);
        t->aging_running = 0;
    }
    pthread_mutex_destroy(&t->aging_lock);
    pthread_cond_destroy(&t->aging_cond);

    munmap(t->s, sizeof(MacShm));
    t->s = NULL;
    /* a clean shutdown removes the object; a crash leaves it for the restart */
    if (t->shm_name[0] != '\0') shm_unlink(t->shm_name);
}

/* ------------------------------------------------------------------ */
/*  Lookup                                                              */
/* ------------------------------------------------------------------ */

int mac_table_lookup(MacTable *t, const uint8_t mac[6])
{
    MacShm *s    = t->s;
    int     b    = mac_hash(mac);
    int     port = -1;

    bl(t, b, 0);
    for (int i = s->head[b]; i >= 0; i = s->pool[i].next) {
        if (memcmp(s->pool[i].mac, mac, 6) == 0) {
            port = s->pool[i].port_idx;   /* aging follows the source only */
            break;
        }
    }
    bu(t, b);
    return port;
}

/* ------------------------------------------------------------------ */
/*  Count MACs on a port (no bucket lock may be held by the caller)     */
/* ------------------------------------------------------------------ */

static int count_macs_on_port(MacTable *t, int port_idx)
{
    MacShm *s = t->s;
    int count = 0;
    for (int b = 0; b < MAC_TABLE_BUCKETS; b++) {
        bl(t, b, 0);
        for (int i = s->head[b]; i >= 0; i = s->pool[i].next)
            if (s->pool[i].port_idx == port_idx) count++;
        bu(t, b);
    }
    return count;
}

/* ------------------------------------------------------------------ */
/*  Evict the least recently used entry                                 */
/* ------------------------------------------------------------------ */

static void evict_lru(MacTable *t)
{
    MacShm  *s = t->s;
    int      oldest_bucket = -1;
    uint8_t  oldest_mac[6] = {0};
    time_t   oldest_time   = (time_t)(-1);

    for (int b = 0; b < MAC_TABLE_BUCKETS; b++) {
        bl(t, b, 0);
        for (int i = s->head[b]; i >= 0; i = s->pool[i].next) {
            if (oldest_bucket == -1 || s->pool[i].last_seen < oldest_time) {
                oldest_time   = s->pool[i].last_seen;
                oldest_bucket = b;
                memcpy(oldest_mac, s->pool[i].mac, 6);
            }
        }
        bu(t, b);
    }

    if (oldest_bucket < 0) return;

    /* the lock was dropped after the scan: find the entry again by MAC */
    int removed = 0;
    bl(t, oldest_bucket, 1);
    int *pp = &s->head[oldest_bucket];
    while (*pp >= 0) {
        MacSlot *e = &s->pool[*pp];
        if (memcmp(e->mac, oldest_mac, 6) == 0 && e->last_seen == oldest_time) {
            int dead = *pp;
            *pp = e->next;
            slot_free(t, dead);
            removed = 1;
            break;
        }
        pp = &e->next;
    }
    bu(t, oldest_bucket);

    if (removed)
        log_push("mac: LRU evict %02x:%02x:%02x:%02x:%02x:%02x",
                 oldest_mac[0], oldest_mac[1], oldest_mac[2],
                 oldest_mac[3], oldest_mac[4], oldest_mac[5]);
}

/* ------------------------------------------------------------------ */
/*  Learn                                                               */
/* ------------------------------------------------------------------ */

int mac_table_learn(MacTable *t, const uint8_t mac[6], int port_idx)
{
    MacShm *s = t->s;
    int     b = mac_hash(mac);

    bl(t, b, 1);
    for (int i = s->head[b]; i >= 0; i = s->pool[i].next) {
        MacSlot *e = &s->pool[i];
        if (memcmp(e->mac, mac, 6) == 0) {
            if (e->port_idx != port_idx) {
                log_push("MAC moved: %02x:%02x:%02x:%02x:%02x:%02x"
                         " port %d -> %d",
                         mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                         e->port_idx, port_idx);
                e->port_idx = port_idx;
            }
            e->last_seen = time(NULL);
            bu(t, b);
            return 0;
        }
    }
    bu(t, b);

    /* new MAC: port security */
    if (count_macs_on_port(t, port_idx) >= MAC_PORT_MAX_MACS) {
        printf("[mac] port %d exceeded MAC limit (%d), shutting down\n",
               port_idx, MAC_PORT_MAX_MACS);
        return -1;
    }

    /* take a free slot; the pool is full when the table is full */
    int n = slot_alloc(t);
    if (n < 0) {
        evict_lru(t);
        n = slot_alloc(t);
        if (n < 0) return 0;          /* lost a race for the freed slot */
    }

    MacSlot *ne = &s->pool[n];
    memcpy(ne->mac, mac, 6);
    ne->port_idx  = port_idx;
    ne->last_seen = time(NULL);

    bl(t, b, 1);
    /* another receiver may have inserted the same MAC since our check */
    for (int i = s->head[b]; i >= 0; i = s->pool[i].next) {
        MacSlot *e = &s->pool[i];
        if (memcmp(e->mac, mac, 6) == 0) {
            e->port_idx  = port_idx;
            e->last_seen = ne->last_seen;
            bu(t, b);
            slot_free(t, n);
            return 0;
        }
    }
    ne->next   = s->head[b];
    s->head[b] = n;
    __atomic_store_n(&ne->used, 1, __ATOMIC_RELEASE);
    bu(t, b);

    log_push("learned %02x:%02x:%02x:%02x:%02x:%02x on port %d",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], port_idx);
    
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Remove entries                                                      */
/* ------------------------------------------------------------------ */

void mac_table_remove_port(MacTable *t, int port_idx)
{
    MacShm *s = t->s;
    for (int b = 0; b < MAC_TABLE_BUCKETS; b++) {
        bl(t, b, 1);
        int *pp = &s->head[b];
        while (*pp >= 0) {
            MacSlot *e = &s->pool[*pp];
            if (e->port_idx == port_idx) {
                int dead = *pp;
                *pp = e->next;
                slot_free(t, dead);
            } else {
                pp = &e->next;
            }
        }
        bu(t, b);
    }
}

void mac_table_clear(MacTable *t)
{
    MacShm *s = t->s;
    for (int b = 0; b < MAC_TABLE_BUCKETS; b++) {
        bl(t, b, 1);
        while (s->head[b] >= 0) {
            int dead = s->head[b];
            s->head[b] = s->pool[dead].next;
            slot_free(t, dead);
        }
        bu(t, b);
    }
}

/* ------------------------------------------------------------------ */
/*  Snapshot (used by "show mac" and the SIGUSR1 dump)                  */
/* ------------------------------------------------------------------ */

int mac_table_snapshot(MacTable *t, MacSnap *out, int max)
{
    MacShm *s   = t->s;
    time_t  now = time(NULL);
    int     n   = 0;
    for (int b = 0; b < MAC_TABLE_BUCKETS; b++) {
        bl(t, b, 0);
        for (int i = s->head[b]; i >= 0 && n < max; i = s->pool[i].next) {
            memcpy(out[n].mac, s->pool[i].mac, 6);
            out[n].port_idx = s->pool[i].port_idx;
            out[n].age      = (long)(now - s->pool[i].last_seen);
            n++;
        }
        bu(t, b);
    }
    return n;
}

/* ------------------------------------------------------------------ */
/*  Aging thread                                                        */
/*  Uses pthread_cond_timedwait so it wakes immediately on shutdown.   */
/* ------------------------------------------------------------------ */

void *mac_table_aging_thread(void *arg)
{
    MacTable *t = (MacTable *)arg;
    MacShm   *s = t->s;

    pthread_mutex_lock(&t->aging_lock);

    while (!t->aging_stop) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += MAC_AGING_SECS;

        pthread_cond_timedwait(&t->aging_cond, &t->aging_lock, &ts);

        if (t->aging_stop) break;

        pthread_mutex_unlock(&t->aging_lock);

        time_t now     = time(NULL);
        int    removed = 0;

        for (int b = 0; b < MAC_TABLE_BUCKETS; b++) {
            bl(t, b, 1);
            int *pp = &s->head[b];
            while (*pp >= 0) {
                MacSlot *e = &s->pool[*pp];
                if ((now - e->last_seen) >= MAC_AGING_SECS) {
                    int dead = *pp;
                    *pp = e->next;
                    slot_free(t, dead);
                    removed++;
                } else {
                    pp = &e->next;
                }
            }
            bu(t, b);
        }

        if (removed > 0)
            log_push("aging removed %d entries", removed);

        pthread_mutex_lock(&t->aging_lock);
    }

    pthread_mutex_unlock(&t->aging_lock);
    return NULL;
}