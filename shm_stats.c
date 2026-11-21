#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <pthread.h>

#include "shm_stats.h"

ShmStats *shm_stats_create(int num_ports)
{
    int       fd;
    ShmStats *stats;

    /* create the shared memory object — O_CREAT|O_EXCL fails if
       it already exists, so unlink first just in case */
    shm_unlink(SHM_NAME);

    fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0600);
    if (fd < 0) {
        perror("shm_open create");
        return NULL;
    }

    /* size the object to fit our struct */
    if (ftruncate(fd, sizeof(ShmStats)) < 0) {
        perror("ftruncate");
        close(fd);
        shm_unlink(SHM_NAME);
        return NULL;
    }

    /* map it into our address space */
    stats = mmap(NULL, sizeof(ShmStats),
                 PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd); /* fd no longer needed after mmap */

    if (stats == MAP_FAILED) {
        perror("mmap create");
        shm_unlink(SHM_NAME);
        return NULL;
    }

    /* zero everything out */
    memset(stats, 0, sizeof(ShmStats));
    stats->num_ports = num_ports;

    /* initialize a process-shared mutex */
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&stats->lock, &attr);
    pthread_mutexattr_destroy(&attr);

    printf("[shm] created %s, %zu bytes\n", SHM_NAME, sizeof(ShmStats));
    return stats;
}

ShmStats *shm_stats_open(void)
{
    int       fd;
    ShmStats *stats;

    /* open existing shared memory — do not create */
    fd = shm_open(SHM_NAME, O_RDWR, 0600);
    if (fd < 0) {
        perror("shm_open open");
        return NULL;
    }

    stats = mmap(NULL, sizeof(ShmStats),
                 PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);

    if (stats == MAP_FAILED) {
        perror("mmap open");
        return NULL;
    }

    return stats;
}

void shm_stats_close(ShmStats *stats)
{
    if (stats)
        munmap(stats, sizeof(ShmStats));
}

void shm_stats_unlink(void)
{
    shm_unlink(SHM_NAME);
    printf("[shm] unlinked %s\n", SHM_NAME);
}

/* --- counter helpers --- */

void shm_inc_in(ShmStats *stats, int port_idx)
{
    if (!stats || port_idx < 0 || port_idx >= MAX_SHM_PORTS) return;
    pthread_mutex_lock(&stats->lock);
    stats->ports[port_idx].frames_in++;
    pthread_mutex_unlock(&stats->lock);
}

void shm_inc_out(ShmStats *stats, int port_idx)
{
    if (!stats || port_idx < 0 || port_idx >= MAX_SHM_PORTS) return;
    pthread_mutex_lock(&stats->lock);
    stats->ports[port_idx].frames_out++;
    pthread_mutex_unlock(&stats->lock);
}

void shm_inc_dropped(ShmStats *stats, int port_idx)
{
    if (!stats || port_idx < 0 || port_idx >= MAX_SHM_PORTS) return;
    pthread_mutex_lock(&stats->lock);
    stats->ports[port_idx].frames_dropped++;
    pthread_mutex_unlock(&stats->lock);
}

void shm_inc_mirrored(ShmStats *stats, int port_idx)
{
    if (!stats || port_idx < 0 || port_idx >= MAX_SHM_PORTS) return;
    pthread_mutex_lock(&stats->lock);
    stats->ports[port_idx].frames_mirrored++;
    pthread_mutex_unlock(&stats->lock);
}