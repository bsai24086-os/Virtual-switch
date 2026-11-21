#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdatomic.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <semaphore.h>
#include <time.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "logger.h"
#include "frame.h"
#include "mac_table.h"
#include "shm_stats.h"
#include "control.h"

/* ------------------------------------------------------------------ */
/*  Configuration                                                       */
/* ------------------------------------------------------------------ */

#define MAX_PORTS        64
#define QUEUE_SIZE       64     /* slots in EACH priority queue          */
#define PORT_WAIT_SECS    5
#define NUM_PRIOS         3     /* 0 = high, 1 = normal, 2 = low         */
#define REFILL_TICK_MS  100     /* token timer period                    */
#define TICKS_PER_SEC   (1000 / REFILL_TICK_MS)

/* Tunables. Set from the config file at start and again on SIGHUP.
 * Atomic so any thread can read them without a lock. */
static atomic_int g_rate_limit         = 0;           /* frames/s/port, 0 = off */
static atomic_int g_aging_ms           = 200;         /* low/normal max wait    */
static atomic_int g_wrr[NUM_PRIOS]     = {5, 3, 1};   /* WRR weights            */

/* ------------------------------------------------------------------ */
/*  Globals                                                             */
/* ------------------------------------------------------------------ */

static atomic_int   g_shutdown    = 0;
static const char  *g_config_path = NULL;
static int          g_listen_fd   = -1;  /* set before any thread starts */
static int          g_verbose     = 0;   /* VSWITCH_VERBOSE=1            */
static sem_t        g_free_ports;
static MacTable     mac_table;
static ShmStats    *g_stats       = NULL;

/* Lets an admin command wait until a port has really been freed.
 * Lock order: state_lock may be held while taking ports_lock (read).
 * Never take state_lock while holding ports_lock. */
static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  state_cond = PTHREAD_COND_INITIALIZER;


static void ts_add_ms(struct timespec *t, long ms)
{
    t->tv_sec  += ms / 1000;
    t->tv_nsec += (ms % 1000) * 1000000L;
    if (t->tv_nsec >= 1000000000L) {
        t->tv_sec++;
        t->tv_nsec -= 1000000000L;
    }
}

static long ms_between(const struct timespec *a, const struct timespec *b)
{
    return (b->tv_sec - a->tv_sec) * 1000L +
           (b->tv_nsec - a->tv_nsec) / 1000000L;
}

/* ------------------------------------------------------------------ */
/*  Config file                                                         */
/*    rate_limit   <fps>        0 = no limit                            */
/*    low_aging_ms <ms>         max wait for normal/low before boost    */
/*    wrr_weights  <h> <n> <l>                                          */
/* ------------------------------------------------------------------ */

static int load_config(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        perror(path);
        return -1;
    }

    char line[256];
    int  lineno = 0;
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        char *hash = strchr(line, '#');
        if (hash) *hash = '\0';

        char key[64];
        int  a = 0, b = 0, c = 0;
        int  n = sscanf(line, "%63s %d %d %d", key, &a, &b, &c);
        if (n <= 0) continue;                    /* blank line */

        if (strcmp(key, "rate_limit") == 0 && n == 2 && a >= 0) {
            atomic_store(&g_rate_limit, a);
        } else if (strcmp(key, "low_aging_ms") == 0 && n == 2 && a > 0) {
            atomic_store(&g_aging_ms, a);
        } else if (strcmp(key, "wrr_weights") == 0 && n == 4 &&
                   a > 0 && b > 0 && c > 0) {
            atomic_store(&g_wrr[0], a);
            atomic_store(&g_wrr[1], b);
            atomic_store(&g_wrr[2], c);
        } else {
            fprintf(stderr, "[config] %s:%d ignored\n", path, lineno);
        }
    }

    if (fclose(f) != 0)
        perror("fclose config");

    printf("[config] rate_limit=%d low_aging_ms=%d wrr=%d:%d:%d\n",
           atomic_load(&g_rate_limit), atomic_load(&g_aging_ms),
           atomic_load(&g_wrr[0]), atomic_load(&g_wrr[1]),
           atomic_load(&g_wrr[2]));
    log_push("config loaded from %s: rate_limit=%d low_aging_ms=%d wrr=%d:%d:%d",
             path, atomic_load(&g_rate_limit), atomic_load(&g_aging_ms),
             atomic_load(&g_wrr[0]), atomic_load(&g_wrr[1]),
             atomic_load(&g_wrr[2]));
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Reference-counted shared frame                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    FrameHeader     header;
    uint8_t        *payload;
    int             refcount;
    pthread_mutex_t reflock;
} SharedFrame;

/* Takes ownership of src->payload. Starts with refcount 1 (the caller). */
static SharedFrame *shared_frame_alloc(Frame *src)
{
    SharedFrame *sf = malloc(sizeof(SharedFrame));
    if (!sf) return NULL;
    if (pthread_mutex_init(&sf->reflock, NULL) != 0) {
        free(sf);
        return NULL;
    }
    sf->header   = src->header;
    sf->payload  = src->payload;
    sf->refcount = 1;
    src->payload = NULL;
    return sf;
}

static void shared_frame_ref(SharedFrame *sf)
{
    pthread_mutex_lock(&sf->reflock);
    sf->refcount++;
    pthread_mutex_unlock(&sf->reflock);
}

static void shared_frame_unref(SharedFrame *sf)
{
    int refs;
    pthread_mutex_lock(&sf->reflock);
    refs = --sf->refcount;
    pthread_mutex_unlock(&sf->reflock);
    if (refs == 0) {
        free(sf->payload);
        pthread_mutex_destroy(&sf->reflock);
        free(sf);
    }
}

/* ------------------------------------------------------------------ */
/*  Single FIFO (no lock of its own: PriorityQueue.lock protects it)    */
/* ------------------------------------------------------------------ */

typedef struct {
    SharedFrame    *sf;
    struct timespec enq;      /* CLOCK_MONOTONIC time it was queued */
} Slot;

typedef struct {
    Slot slots[QUEUE_SIZE];
    int  head;
    int  tail;
    int  count;
} SingleQueue;

static SharedFrame *sq_pop(SingleQueue *q)
{
    if (q->count == 0) return NULL;
    SharedFrame *sf = q->slots[q->head].sf;
    q->head = (q->head + 1) % QUEUE_SIZE;
    q->count--;
    return sf;
}

/* ------------------------------------------------------------------ */
/*  Priority queue: 3 FIFOs + weighted round robin + aging (item 6)     */
/* ------------------------------------------------------------------ */

typedef struct {
    SingleQueue     q[NUM_PRIOS];
    int             credits[NUM_PRIOS];  /* WRR credits left this round */
    int             closing;             /* port going down             */
    uint64_t        dropped;             /* queue-full drops            */
    uint64_t        aged;                /* frames sent by aging rule   */
    int             last_aged;           /* previous pick was by aging  */
    pthread_mutex_t lock;
    pthread_cond_t  not_empty;
} PriorityQueue;

static void pq_refill_credits(PriorityQueue *pq)
{
    for (int i = 0; i < NUM_PRIOS; i++)
        pq->credits[i] = atomic_load(&g_wrr[i]);
}

static int pq_init(PriorityQueue *pq)
{
    memset(pq, 0, sizeof(PriorityQueue));
    pq_refill_credits(pq);
    if (pthread_mutex_init(&pq->lock, NULL) != 0) return -1;
    if (pthread_cond_init(&pq->not_empty, NULL) != 0) return -1;
    return 0;
}

static void pq_destroy(PriorityQueue *pq)
{
    pthread_mutex_destroy(&pq->lock);
    pthread_cond_destroy(&pq->not_empty);
}

/* Drop every queued frame and make the queue ready for a new host. */
static void pq_reset(PriorityQueue *pq)
{
    pthread_mutex_lock(&pq->lock);
    for (int i = 0; i < NUM_PRIOS; i++) {
        SharedFrame *sf;
        while ((sf = sq_pop(&pq->q[i])) != NULL)
            shared_frame_unref(sf);
        pq->q[i].head = pq->q[i].tail = 0;
    }
    pq_refill_credits(pq);
    pq->closing = 0;
    pq->dropped = 0;
    pq->aged    = 0;
    pq->last_aged = 0;
    pthread_mutex_unlock(&pq->lock);
}

/* Drop every queued frame but keep the queue running (VLAN change). */
static void pq_flush(PriorityQueue *pq)
{
    pthread_mutex_lock(&pq->lock);
    for (int i = 0; i < NUM_PRIOS; i++) {
        SharedFrame *sf;
        while ((sf = sq_pop(&pq->q[i])) != NULL)
            shared_frame_unref(sf);
    }
    pthread_mutex_unlock(&pq->lock);
}

/* Never blocks. Returns 1 if queued, 0 if dropped (full or closing). */
static int pq_push(PriorityQueue *pq, SharedFrame *sf)
{
    int prio = sf->header.priority;
    if (prio >= NUM_PRIOS) prio = 1;       /* bad value: treat as normal */

    int ok = 0;
    pthread_mutex_lock(&pq->lock);
    SingleQueue *q = &pq->q[prio];
    if (pq->closing || q->count >= QUEUE_SIZE) {
        pq->dropped++;
    } else {
        q->slots[q->tail].sf = sf;
        clock_gettime(CLOCK_MONOTONIC, &q->slots[q->tail].enq);
        q->tail = (q->tail + 1) % QUEUE_SIZE;
        q->count++;
        ok = 1;
        pthread_cond_signal(&pq->not_empty);
    }
    pthread_mutex_unlock(&pq->lock);
    return ok;
}

/*
 * Blocks until a frame is ready or the port is closing (returns NULL).
 *  1. Aging: if the oldest LOW frame has waited longer than
 *     low_aging_ms, send it next, whatever the weights say. Never two
 *     aged picks in a row: otherwise a small threshold lets the low
 *     queue take the whole link and HIGH starves instead.
 *  2. Otherwise weighted round robin: take from the highest priority
 *     queue that has frames and credits left. When no non-empty queue
 *     has credits, start a new round.
 */
static SharedFrame *pq_pop(PriorityQueue *pq)
{
    SharedFrame *sf = NULL;

    pthread_mutex_lock(&pq->lock);
    for (;;) {
        if (pq->closing) break;

        int total = 0;
        for (int i = 0; i < NUM_PRIOS; i++) total += pq->q[i].count;
        if (total == 0) {
            pthread_cond_wait(&pq->not_empty, &pq->lock);
            continue;
        }

        /* 1. aging */
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long limit = atomic_load(&g_aging_ms);
        SingleQueue *low = &pq->q[NUM_PRIOS - 1];
        if (!pq->last_aged && low->count > 0 &&
            ms_between(&low->slots[low->head].enq, &now) >= limit) {
            sf = sq_pop(low);
            pq->aged++;
            pq->last_aged = 1;
            break;
        }

        /* 2. weighted round robin */
        for (int i = 0; i < NUM_PRIOS; i++) {
            if (pq->q[i].count > 0 && pq->credits[i] > 0) {
                sf = sq_pop(&pq->q[i]);
                pq->credits[i]--;
                pq->last_aged = 0;
                break;
            }
        }
        if (sf) break;
        pq_refill_credits(pq);            /* round over, start the next */
    }
    pthread_mutex_unlock(&pq->lock);
    return sf;
}

/* ------------------------------------------------------------------ */
/*  Token bucket (item 7)                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    int             tokens;
    int             carry;      /* fractional tokens, in 1/TICKS_PER_SEC */
    int             closing;
    pthread_mutex_t lock;
    pthread_cond_t  has_token;
} TokenBucket;

static int tb_init(TokenBucket *tb)
{
    memset(tb, 0, sizeof(TokenBucket));
    if (pthread_mutex_init(&tb->lock, NULL) != 0) return -1;
    if (pthread_cond_init(&tb->has_token, NULL) != 0) return -1;
    return 0;
}

static void tb_destroy(TokenBucket *tb)
{
    pthread_mutex_destroy(&tb->lock);
    pthread_cond_destroy(&tb->has_token);
}

static void tb_reset(TokenBucket *tb)
{
    pthread_mutex_lock(&tb->lock);
    tb->tokens  = atomic_load(&g_rate_limit);   /* start with a full bucket */
    tb->carry   = 0;
    tb->closing = 0;
    pthread_mutex_unlock(&tb->lock);
}

/* Take one token. Waits with pthread_cond_timedwait while the bucket is
 * empty. Returns 1 when a token was taken, 0 if the port is closing. */
static int tb_consume(TokenBucket *tb)
{
    int ok = 1;
    pthread_mutex_lock(&tb->lock);
    for (;;) {
        if (tb->closing) { ok = 0; break; }
        if (atomic_load(&g_rate_limit) <= 0) break;     /* no limit */
        if (tb->tokens > 0) { tb->tokens--; break; }

        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 1;                                 /* safety net only */
        pthread_cond_timedwait(&tb->has_token, &tb->lock, &ts);
    }
    pthread_mutex_unlock(&tb->lock);
    return ok;
}

/* Called by the timer thread every tick. Adds rate/TICKS_PER_SEC tokens,
 * keeping the remainder so that low rates are exact. Burst = rate. */
static void tb_refill(TokenBucket *tb, int rate)
{
    pthread_mutex_lock(&tb->lock);
    if (rate > 0) {
        tb->carry += rate;
        int add = tb->carry / TICKS_PER_SEC;
        tb->carry %= TICKS_PER_SEC;
        tb->tokens += add;
        if (tb->tokens > rate) tb->tokens = rate;
        if (add > 0) pthread_cond_broadcast(&tb->has_token);
    } else {
        pthread_cond_broadcast(&tb->has_token);   /* limit turned off */
    }
    pthread_mutex_unlock(&tb->lock);
}

/* ------------------------------------------------------------------ */
/*  Port table                                                          */
/* ------------------------------------------------------------------ */

enum { PORT_FREE = 0, PORT_UP, PORT_CLOSING };

typedef struct {
    int           fd;         /* -1 when free                        */
    int           state;      /* PORT_FREE / PORT_UP / PORT_CLOSING  */
    int           mirror_to;  /* sniffer port for ingress, -1 = off  */
    PriorityQueue pq;
    TokenBucket   tb;
    int           vlan;       /* VLAN of a normal port, 1..4094      */
    int           is_trunk;   /* 1 = carries every VLAN              */
    int           admin_down; /* swctl shutdown: accept loop skips it */
    pthread_t     sender_tid;
} PortState;

/* ports_lock protects fd, state and mirror_to of every port.
 * Forwarding takes it for reading; add/remove/mirror take it for writing. */
static PortState        ports[MAX_PORTS];
static int              num_ports = 0;        /* fixed after startup */
static pthread_rwlock_t ports_lock = PTHREAD_RWLOCK_INITIALIZER;

/* Only the main thread touches these two. */
static pthread_t handler_tid[MAX_PORTS];
static int       handler_joinable[MAX_PORTS];

/* ------------------------------------------------------------------ */
/*  Token timer thread                                                  */
/* ------------------------------------------------------------------ */

static pthread_mutex_t timer_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  timer_cond = PTHREAD_COND_INITIALIZER;
static int             timer_stop = 0;           /* under timer_lock */

static void *token_refill_thread(void *arg)
{
    (void)arg;
    struct timespec next;
    clock_gettime(CLOCK_REALTIME, &next);

    pthread_mutex_lock(&timer_lock);
    while (!timer_stop) {
        ts_add_ms(&next, REFILL_TICK_MS);
        int r = 0;
        while (!timer_stop && r != ETIMEDOUT)
            r = pthread_cond_timedwait(&timer_cond, &timer_lock, &next);
        if (timer_stop) break;
        pthread_mutex_unlock(&timer_lock);

        int rate = atomic_load(&g_rate_limit);
        pthread_rwlock_rdlock(&ports_lock);
        for (int i = 0; i < num_ports; i++)
            if (ports[i].state == PORT_UP)
                tb_refill(&ports[i].tb, rate);
        pthread_rwlock_unlock(&ports_lock);

        pthread_mutex_lock(&timer_lock);
    }
    pthread_mutex_unlock(&timer_lock);
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  port_set_mirror: called by the control thread                       */
/* ------------------------------------------------------------------ */

void port_set_mirror(int src, int dst)
{
    pthread_rwlock_wrlock(&ports_lock);
    if (src >= 0 && src < num_ports)
        ports[src].mirror_to = dst;
    pthread_rwlock_unlock(&ports_lock);
    log_push("mirror: port %d -> port %d", src, dst);
}

/* ------------------------------------------------------------------ */
/*  Live reconfiguration (called by the control thread)                 */
/* ------------------------------------------------------------------ */

/* Waits up to 2 s for port idx to be completely free. 1 = free. */
static int wait_port_free(int idx)
{
    struct timespec ts;
    int             is_free;

    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 2;

    pthread_mutex_lock(&state_lock);
    for (;;) {
        pthread_rwlock_rdlock(&ports_lock);
        is_free = (ports[idx].state == PORT_FREE);
        pthread_rwlock_unlock(&ports_lock);
        if (is_free) break;
        if (pthread_cond_timedwait(&state_cond, &state_lock, &ts) == ETIMEDOUT) {
            pthread_rwlock_rdlock(&ports_lock);
            is_free = (ports[idx].state == PORT_FREE);
            pthread_rwlock_unlock(&ports_lock);
            break;
        }
    }
    pthread_mutex_unlock(&state_lock);
    return is_free;
}

/* Shut a port down and keep it down. A connected host is cut off: its
 * receiver thread wakes, joins the sender, drains the queue and frees
 * the port (port_remove). Returns 0 when done, -1 bad port, -2 timeout. */
int port_admin_down(int idx)
{
    if (idx < 0 || idx >= num_ports) return -1;

    pthread_rwlock_wrlock(&ports_lock);
    ports[idx].admin_down = 1;
    if (ports[idx].state == PORT_UP)
        shutdown(ports[idx].fd, SHUT_RDWR);        /* wakes the receiver */
    pthread_rwlock_unlock(&ports_lock);

    int ok = wait_port_free(idx);
    log_push("admin: port %d shut down%s", idx, ok ? "" : " (still closing)");
    return ok ? 0 : -2;
}

/* Allow hosts to use the port again. */
int port_admin_up(int idx)
{
    if (idx < 0 || idx >= num_ports) return -1;

    pthread_rwlock_wrlock(&ports_lock);
    ports[idx].admin_down = 0;
    pthread_rwlock_unlock(&ports_lock);

    log_push("admin: port %d up", idx);
    return 0;
}

/* Move a port to another VLAN while traffic flows. */
int port_set_vlan(int idx, int vlan)
{
    if (idx < 0 || idx >= num_ports || vlan < 1 || vlan > 4094) return -1;

    pthread_rwlock_wrlock(&ports_lock);
    ports[idx].vlan = vlan;
    pthread_rwlock_unlock(&ports_lock);

    pq_flush(&ports[idx].pq);      /* frames from the old VLAN must not leak */
    log_push("admin: port %d moved to VLAN %d", idx, vlan);
    return 0;
}

int port_set_trunk(int idx, int on)
{
    if (idx < 0 || idx >= num_ports) return -1;

    pthread_rwlock_wrlock(&ports_lock);
    ports[idx].is_trunk = on ? 1 : 0;
    pthread_rwlock_unlock(&ports_lock);

    pq_flush(&ports[idx].pq);
    log_push("admin: port %d trunk %s", idx, on ? "on" : "off");
    return 0;
}

void port_get_info(int idx, PortInfo *out)
{
    pthread_rwlock_rdlock(&ports_lock);
    out->vlan  = ports[idx].vlan;
    out->trunk = ports[idx].is_trunk;
    if (ports[idx].state == PORT_UP && !ports[idx].admin_down)
        strcpy(out->state, "up");
    else if (ports[idx].state == PORT_FREE && ports[idx].admin_down)
        strcpy(out->state, "down");
    else if (ports[idx].state == PORT_FREE)
        strcpy(out->state, "free");
    else
        strcpy(out->state, "closing");
    pthread_rwlock_unlock(&ports_lock);
}

/* ------------------------------------------------------------------ */
/*  Sender thread: one per port                                         */
/* ------------------------------------------------------------------ */

static void *sender_thread(void *arg)
{
    int        idx = (int)(intptr_t)arg;
    PortState *p   = &ports[idx];

    for (;;) {
        SharedFrame *sf = pq_pop(&p->pq);
        if (!sf) break;                          /* port closing */

        if (!tb_consume(&p->tb)) {               /* closing while waiting */
            shared_frame_unref(sf);
            break;
        }

        Frame tmp;
        tmp.header  = sf->header;
        tmp.payload = sf->payload;
        int w = frame_write(p->fd, &tmp);
        shared_frame_unref(sf);

        if (w < 0) {
            /* host gone (EPIPE): wake our receiver so it removes the port */
            log_push("port %d: write failed, closing", idx);
            shutdown(p->fd, SHUT_RDWR);
            break;
        }
        shm_inc_out(g_stats, idx);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Port add / remove                                                   */
/* ------------------------------------------------------------------ */

/* Returns the port index, or -1. Caller owns fd on failure. */
static int port_add(int fd)
{
    int idx = -1;

    pthread_rwlock_wrlock(&ports_lock);
    if (!atomic_load(&g_shutdown)) {
        for (int i = 0; i < num_ports; i++) {
            if (ports[i].state == PORT_FREE && !ports[i].admin_down) {
                idx = i;
                break;
            }
        }
    }
    if (idx != -1) {
        tb_reset(&ports[idx].tb);
        ports[idx].fd        = fd;
        ports[idx].mirror_to = -1;
        ports[idx].state     = PORT_UP;
    }
    pthread_rwlock_unlock(&ports_lock);
    if (idx == -1) return -1;

    if (pthread_create(&ports[idx].sender_tid, NULL, sender_thread,
                       (void *)(intptr_t)idx) != 0) {
        perror("pthread_create sender");
        pthread_rwlock_wrlock(&ports_lock);
        ports[idx].state = PORT_CLOSING;         /* stop new pushes */
        pthread_rwlock_unlock(&ports_lock);
        pq_reset(&ports[idx].pq);
        pthread_rwlock_wrlock(&ports_lock);
        ports[idx].fd    = -1;
        ports[idx].state = PORT_FREE;
        pthread_rwlock_unlock(&ports_lock);
        return -1;
    }
    return idx;
}

/* Called by the port's own receiver thread when it is done.
 * Lock order: ports_lock, then pq.lock / tb.lock, never the reverse. */
static void port_remove(int idx)
{
    PortState *p = &ports[idx];

    /* 1. no new frames for this port, and nobody mirrors to it */
    pthread_rwlock_wrlock(&ports_lock);
    p->state     = PORT_CLOSING;
    p->mirror_to = -1;
    for (int i = 0; i < num_ports; i++)
        if (ports[i].mirror_to == idx) ports[i].mirror_to = -1;
    pthread_rwlock_unlock(&ports_lock);

    /* 2. wake the sender wherever it sleeps, then join it */
    pthread_mutex_lock(&p->pq.lock);
    p->pq.closing = 1;
    pthread_cond_broadcast(&p->pq.not_empty);
    pthread_mutex_unlock(&p->pq.lock);

    pthread_mutex_lock(&p->tb.lock);
    p->tb.closing = 1;
    pthread_cond_broadcast(&p->tb.has_token);
    pthread_mutex_unlock(&p->tb.lock);

    pthread_join(p->sender_tid, NULL);

    /* 3. forget its MACs, drop queued frames, free the slot */
    mac_table_remove_port(&mac_table, idx);

    pthread_mutex_lock(&p->pq.lock);
    uint64_t dropped = p->pq.dropped;
    uint64_t aged    = p->pq.aged;
    pthread_mutex_unlock(&p->pq.lock);

    pq_reset(&p->pq);

    pthread_rwlock_wrlock(&ports_lock);
    int fd   = p->fd;
    p->fd    = -1;
    p->state = PORT_FREE;
    pthread_rwlock_unlock(&ports_lock);

    if (close(fd) < 0) perror("close port fd");

    printf("[port %d] removed, dropped=%lu aged=%lu\n",
           idx, (unsigned long)dropped, (unsigned long)aged);
    log_push("port %d: removed, dropped=%lu aged=%lu",
             idx, (unsigned long)dropped, (unsigned long)aged);

    pthread_mutex_lock(&state_lock);
    pthread_cond_broadcast(&state_cond);       /* wake "swctl shutdown" */
    pthread_mutex_unlock(&state_lock);

    sem_post(&g_free_ports);
}

/* ------------------------------------------------------------------ */
/*  Forwarding                                                          */
/* ------------------------------------------------------------------ */
/* Caller holds ports_lock for reading. Never blocks. */
static int enqueue_to(int i, SharedFrame *sf)
{
    shared_frame_ref(sf);
    if (!pq_push(&ports[i].pq, sf)) {
        shared_frame_unref(sf);
        shm_inc_dropped(g_stats, i);
        if (g_verbose) log_push("port %d: queue full, frame dropped", i);
        return 0;
    }
    return 1;
}

/* Caller holds ports_lock for reading. A port may send a frame of VLAN v
 * when it is up and is either a trunk or a member of v. */
static int egress_ok(int i, int v)
{
    return ports[i].state == PORT_UP &&
           (ports[i].is_trunk || ports[i].vlan == v);
}

/* dst_port == -1 means unknown or broadcast. Takes ownership of
 * frame->payload. */
static void forward(int in_idx, Frame *frame, int dst_port)
{
    SharedFrame *sf = shared_frame_alloc(frame);
    if (!sf) {
        free(frame->payload);
        frame->payload = NULL;
        return;
    }

    pthread_rwlock_rdlock(&ports_lock);

    /* A normal port stamps its own VLAN on the frame. A trunk port keeps
     * the vlan field the other side wrote. */
    int v = ports[in_idx].is_trunk ? ntohs(sf->header.vlan)
                                   : ports[in_idx].vlan;
    if (v < 1 || v > 4094) {
        pthread_rwlock_unlock(&ports_lock);
        shm_inc_dropped(g_stats, in_idx);
        shared_frame_unref(sf);
        return;
    }
    sf->header.vlan = htons((uint16_t)v);    /* sf is still private here */

    int mirror     = ports[in_idx].mirror_to;
    int mirror_got = 0;          /* sniffer already got it normally */
    int handled    = 0;

    if (dst_port == in_idx) {
        handled = 1;             /* same segment: filter */
    } else if (dst_port >= 0 && egress_ok(dst_port, v)) {
        int ok = enqueue_to(dst_port, sf);
        if (dst_port == mirror && ok) mirror_got = 1;
        handled = 1;
    }
    if (!handled) {              /* flood, but only inside the VLAN */
        for (int i = 0; i < num_ports; i++) {
            if (i == in_idx || !egress_ok(i, v)) continue;
            int ok = enqueue_to(i, sf);
            if (i == mirror && ok) mirror_got = 1;
        }
    }

    if (mirror >= 0 && mirror != in_idx && ports[mirror].state == PORT_UP) {
        if (mirror_got || enqueue_to(mirror, sf))
            shm_inc_mirrored(g_stats, in_idx);
    }
    pthread_rwlock_unlock(&ports_lock);

    shared_frame_unref(sf);      /* drop the receiver's own reference */
}

/* ------------------------------------------------------------------ */
/*  Receiver thread: one per port                                       */
/* ------------------------------------------------------------------ */

static const uint8_t BROADCAST[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

static void *port_handler(void *arg)
{
    int idx = (int)(intptr_t)arg;
    int fd  = ports[idx].fd;     /* stable until we call port_remove */

    printf("[port %d] host connected (fd=%d)\n", idx, fd);
    log_push("port %d: host connected", idx);

    for (;;) {
        Frame frame;
        frame.payload = NULL;

        int r = frame_read(fd, &frame);
        if (r == 0) {
            log_push("port %d: host disconnected", idx);
            break;
        }
        if (r < 0) {
            if (!atomic_load(&g_shutdown))
                log_push("port %d: read error or bad frame, closing", idx);
            break;
        }

        shm_inc_in(g_stats, idx);

        if (g_verbose) {
            printf("[port %d] received: ", idx);
            frame_print(&frame);
        }

        if (mac_table_learn(&mac_table, frame.header.src, idx) < 0) {
            printf("[port %d] port security: too many MACs, shutting port\n", idx);
            log_push("port %d: port security triggered, port shut down", idx);
            free(frame.payload);
            break;
        }

        int dst_port = -1;
        if (memcmp(frame.header.dst, BROADCAST, 6) != 0)
            dst_port = mac_table_lookup(&mac_table, frame.header.dst);

        forward(idx, &frame, dst_port);
    }

    port_remove(idx);
    printf("[port %d] host gone\n", idx);
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Signal thread (item 12)                                             */
/* ------------------------------------------------------------------ */
static void dump_mac_table(void)
{
    const char *path = "mac_dump.txt";
    FILE *f = fopen(path, "w");
    if (!f) { perror("fopen mac_dump.txt"); return; }

    time_t    now = time(NULL);
    struct tm tm;
    char      ts[64];
    localtime_r(&now, &tm);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);
    fprintf(f, "=== MAC table dump: %s\n", ts);

    static MacSnap snap[MAC_TABLE_MAX];     /* only the signal thread uses it */
    int n = mac_table_snapshot(&mac_table, snap, MAC_TABLE_MAX);
    for (int i = 0; i < n; i++)
        fprintf(f, "  %02x:%02x:%02x:%02x:%02x:%02x  port %d  age %lds\n",
                snap[i].mac[0], snap[i].mac[1], snap[i].mac[2],
                snap[i].mac[3], snap[i].mac[4], snap[i].mac[5],
                snap[i].port_idx, snap[i].age);

    if (fclose(f) != 0) perror("fclose mac_dump.txt");
    printf("[signal] MAC table dumped to %s\n", path);
    log_push("SIGUSR1: MAC table dumped to %s", path);
}

static void *signal_thread(void *arg)
{
    sigset_t *mask = (sigset_t *)arg;
    int       sig;

    for (;;) {
        if (sigwait(mask, &sig) != 0) continue;

        if (sig == SIGUSR1) {
            dump_mac_table();
            continue;
        }
        if (sig == SIGHUP) {
            if (g_config_path) {
                printf("[signal] SIGHUP: reloading %s\n", g_config_path);
                load_config(g_config_path);
            } else {
                printf("[signal] SIGHUP: no config file given\n");
            }
            continue;
        }

        /* SIGINT / SIGTERM: start a clean shutdown */
        printf("\n[switch] signal %d received, shutting down\n", sig);
        log_push("signal %d: shutting down", sig);
        atomic_store(&g_shutdown, 1);

        shutdown(g_listen_fd, SHUT_RDWR);        /* wakes accept()   */

        pthread_rwlock_rdlock(&ports_lock);      /* wakes every read */
        for (int i = 0; i < num_ports; i++)
            if (ports[i].fd != -1)
                shutdown(ports[i].fd, SHUT_RDWR);
        pthread_rwlock_unlock(&ports_lock);

        sem_post(&g_free_ports);                 /* wakes sem_timedwait */
        break;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Main                                                                */
/* ------------------------------------------------------------------ */

static int parse_int(const char *s, int lo, int hi, int *out)
{
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno || *end != '\0' || v < lo || v > hi) return -1;
    *out = (int)v;
    return 0;
}
int main(int argc, char *argv[])
{
    int tcp_port;

    if (argc < 3 || argc > 4) {
        fprintf(stderr, "usage: ./switch <tcp_port> <num_ports> [config_file]\n");
        return 1;
    }
    if (parse_int(argv[1], 1, 65535, &tcp_port) < 0) {
        fprintf(stderr, "bad tcp_port\n");
        return 1;
    }
    if (parse_int(argv[2], 1, MAX_PORTS, &num_ports) < 0) {
        fprintf(stderr, "num_ports must be 1..%d\n", MAX_PORTS);
        return 1;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);    /* line-buffered even into a file */
    const char *v = getenv("VSWITCH_VERBOSE");
    g_verbose = (v && strcmp(v, "0") != 0);

    if (argc == 4) {
        g_config_path = argv[3];
        if (load_config(g_config_path) < 0) return 1;
    }

    /*
     * A shell starts background jobs with SIGINT ignored, and Linux drops
     * an ignored signal even if we are waiting for it in sigwait(). So put
     * the default action back first, then block. (Found when "./switch &"
     * from a script never reacted to kill -INT.)
     */
    int sigs[] = { SIGINT, SIGTERM, SIGHUP, SIGUSR1 };
    for (size_t i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++) {
        if (signal(sigs[i], SIG_DFL) == SIG_ERR) {
            perror("signal");
            return 1;
        }
    }

    /* block the signals before any thread exists; all threads inherit */
    static sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGHUP);
    sigaddset(&mask, SIGUSR1);
    if (pthread_sigmask(SIG_BLOCK, &mask, NULL) != 0) {
        fprintf(stderr, "pthread_sigmask failed\n");
        return 1;
    }
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        perror("signal SIGPIPE");
        return 1;
    }

    /* listening socket first, so a bind failure exits with nothing running */
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { perror("socket"); return 1; }
    int opt = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt"); close(listen_fd); return 1;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons((uint16_t)tcp_port);
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(listen_fd); return 1;
    }
    if (listen(listen_fd, 16) < 0) {
        perror("listen"); close(listen_fd); return 1;
    }
    g_listen_fd = listen_fd;

    for (int i = 0; i < num_ports; i++) {
        ports[i].fd        = -1;
        ports[i].state     = PORT_FREE;
        ports[i].mirror_to = -1;
        ports[i].vlan      = 1;
        if (pq_init(&ports[i].pq) != 0 || tb_init(&ports[i].tb) != 0) {
            fprintf(stderr, "queue init failed\n");
            return 1;
        }
    }

    g_stats = shm_stats_create(num_ports);
    if (!g_stats) {
        fprintf(stderr, "[switch] failed to create shared memory\n");
        return 1;
    }
    if (sem_init(&g_free_ports, 0, (unsigned)num_ports) != 0) {
        perror("sem_init"); return 1;
    }

    /* under the supervisor (VSWITCH_PERSIST=1) the MAC table outlives a crash */
    const char *persist_env = getenv("VSWITCH_PERSIST");
    int persist = persist_env != NULL && strcmp(persist_env, "1") == 0;
    if (mac_table_init(&mac_table, MAC_SHM_NAME, persist) != 0) {
        fprintf(stderr, "[switch] failed to create MAC table\n");
        return 1;
    }
    if (logger_init() != 0) {
        fprintf(stderr, "logger_init failed\n"); return 1;
    }
    if (mac_table_start_aging(&mac_table) != 0)
        return 1;
    pthread_t sig_tid;
    if (pthread_create(&sig_tid, NULL, signal_thread, &mask) != 0) {
        perror("pthread_create signal"); return 1;
    }
    pthread_t refill_tid;
    if (pthread_create(&refill_tid, NULL, token_refill_thread, NULL) != 0) {
        perror("pthread_create refill"); return 1;
    }
    ControlContext ctrl_ctx = {
        .stats     = g_stats,
        .mac_table = &mac_table,
        .num_ports = num_ports,
    };
    if (control_start(&ctrl_ctx) != 0) {
        fprintf(stderr, "[switch] failed to start control thread\n");
        return 1;
    }

    printf("switch ready on TCP %d, %d ports, rate_limit=%d\n",
           tcp_port, num_ports, atomic_load(&g_rate_limit));
    log_push("switch started on TCP %d with %d ports", tcp_port, num_ports);

    /* ---- accept loop ---- */
    while (!atomic_load(&g_shutdown)) {
        int client_fd = accept(listen_fd, NULL, NULL);
        if (client_fd < 0) {
            if (atomic_load(&g_shutdown)) break;
            if (errno == EINTR || errno == ECONNABORTED) continue;
            perror("accept");
            break;
        }

        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += PORT_WAIT_SECS;

        int got;
        while ((got = sem_timedwait(&g_free_ports, &ts)) < 0 && errno == EINTR)
            ;
        if (got < 0) {
            if (errno == ETIMEDOUT) {
                printf("[switch] no free port, rejecting host\n");
                log_push("switch full: host rejected");
                const char *msg = "switch full\n";
                if (write(client_fd, msg, strlen(msg)) < 0)
                    perror("write switch full");
            } else {
                perror("sem_timedwait");
            }
            close(client_fd);
            continue;
        }
        if (atomic_load(&g_shutdown)) {
            sem_post(&g_free_ports);
            close(client_fd);
            break;
        }

        int idx = port_add(client_fd);
        if (idx == -1) {
            /* every free port is shut down by the admin: nothing to wait for */
            const char *msg = "switch full\n";
            if (write(client_fd, msg, strlen(msg)) < 0)
                perror("write switch full");
            sem_post(&g_free_ports);
            close(client_fd);
            continue;
        }

        /* the slot's previous receiver has already freed it: reap it */
        if (handler_joinable[idx]) {
            pthread_join(handler_tid[idx], NULL);
            handler_joinable[idx] = 0;
        }

        if (pthread_create(&handler_tid[idx], NULL, port_handler,
                           (void *)(intptr_t)idx) != 0) {
            perror("pthread_create handler");
            pthread_rwlock_rdlock(&ports_lock);
            shutdown(client_fd, SHUT_RDWR);
            pthread_rwlock_unlock(&ports_lock);
            port_remove(idx);                    /* closes fd, posts sem */
            continue;
        }
        handler_joinable[idx] = 1;
    }

    /* ---- shutdown sequence ---- */
    if (!atomic_load(&g_shutdown))
        kill(getpid(), SIGTERM);                 /* fatal accept error */

    pthread_join(sig_tid, NULL);                 /* it has shut every socket */
    close(listen_fd);

    for (int i = 0; i < num_ports; i++) {
        if (handler_joinable[i]) {
            pthread_join(handler_tid[i], NULL);
            handler_joinable[i] = 0;
        }
    }

    control_stop();

    pthread_mutex_lock(&timer_lock);
    timer_stop = 1;
    pthread_cond_broadcast(&timer_cond);
    pthread_mutex_unlock(&timer_lock);
    pthread_join(refill_tid, NULL);

    mac_table_destroy(&mac_table);               /* joins the aging thread */

    logger_shutdown();
    logger_join();

    for (int i = 0; i < num_ports; i++) {
        pq_destroy(&ports[i].pq);
        tb_destroy(&ports[i].tb);
    }
    sem_destroy(&g_free_ports);
    pthread_rwlock_destroy(&ports_lock);

    shm_stats_close(g_stats);
    shm_stats_unlink();

    printf("[switch] shutdown complete\n");
    return 0;
}