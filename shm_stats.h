#define _POSIX_C_SOURCE 200809L

#ifndef SHM_STATS_H
#define SHM_STATS_H

#include <stdint.h>
#include <pthread.h>

#define SHM_NAME      "/vswitch_stats"   /* name in /dev/shm */
#define MAX_SHM_PORTS 64                 /* max ports we track */

/*
 * Per-port counters.
 * One of these exists for every port slot.
 */
typedef struct {
    uint64_t frames_in;       /* frames received from this host */
    uint64_t frames_out;      /* frames forwarded out this port */
    uint64_t frames_dropped;  /* frames dropped (queue full etc) */
    uint64_t frames_mirrored; /* frames copied to mirror port */
} PortCounters;

/*
 * The full shared memory block.
 * One instance lives in /dev/shm/vswitch_stats.
 * Both switch and swctl mmap this same block.
 */
typedef struct {
    pthread_mutex_t lock;                   /* process-shared mutex */
    int             num_ports;              /* how many ports exist */
    PortCounters    ports[MAX_SHM_PORTS];   /* one entry per port */
} ShmStats;

/*
 * Called by the switch (creator).
 * Creates the shared memory object, sizes it, and mmaps it.
 * Initializes the mutex with PTHREAD_PROCESS_SHARED.
 * Returns a pointer to the mapped ShmStats, or NULL on error.
 */
ShmStats *shm_stats_create(int num_ports);

/*
 * Called by swctl (reader).
 * Opens an existing shared memory object and mmaps it.
 * Returns a pointer to the mapped ShmStats, or NULL on error.
 */
ShmStats *shm_stats_open(void);

/*
 * Detach from the shared memory (both creator and reader call this).
 */
void shm_stats_close(ShmStats *stats);

/*
 * Called by the switch on shutdown.
 * Removes the shared memory object from /dev/shm.
 */
void shm_stats_unlink(void);

/*
 * Increment frames_in for a port (called by switch on frame receive).
 */
void shm_inc_in(ShmStats *stats, int port_idx);

/*
 * Increment frames_out for a port (called by switch on frame forward).
 */
void shm_inc_out(ShmStats *stats, int port_idx);

/*
 * Increment frames_dropped for a port.
 */
void shm_inc_dropped(ShmStats *stats, int port_idx);

/*
 * Increment frames_mirrored for a port.
 */
void shm_inc_mirrored(ShmStats *stats, int port_idx);

#endif /* SHM_STATS_H */