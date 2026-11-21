#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <time.h>
#include <pthread.h>

#include "logger.h"

/* one log message slot */
typedef struct {
    char msg[512];
} LogEntry;

/* the queue */
static LogEntry      log_queue[LOG_QUEUE_MAX];
static int           log_head     = 0;
static int           log_tail     = 0;
static int           log_count    = 0;
static int           log_shutdown = 0;
static uint64_t      log_dropped  = 0;

static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  log_cond = PTHREAD_COND_INITIALIZER;
static pthread_t       log_tid;

/* --- logger thread --- */
static void *logger_thread(void *arg)
{
    (void)arg;
    FILE *f = fopen(LOG_FILE, "a");
    if (!f) {
        perror("logger fopen");
        return NULL;
    }

    printf("[logger] started, writing to %s\n", LOG_FILE);

    while (1) {
        pthread_mutex_lock(&log_lock);

        /* wait until there is something to write or shutdown */
        while (log_count == 0 && !log_shutdown)
            pthread_cond_wait(&log_cond, &log_lock);

        /* drain everything currently in the queue */
        while (log_count > 0) {
            LogEntry entry = log_queue[log_head];
            log_head = (log_head + 1) % LOG_QUEUE_MAX;
            log_count--;
            pthread_mutex_unlock(&log_lock);

            /* write timestamp + message to file */
            time_t    now = time(NULL);
            struct tm tm;
            char      ts[32];
            localtime_r(&now, &tm);        /* localtime() is not thread-safe */
            strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);
            fprintf(f, "[%s] %s\n", ts, entry.msg);
            fflush(f);

            pthread_mutex_lock(&log_lock);
        }

        if (log_shutdown && log_count == 0) {
            pthread_mutex_unlock(&log_lock);
            break;
        }

        pthread_mutex_unlock(&log_lock);
    }

    pthread_mutex_lock(&log_lock);
    uint64_t dropped = log_dropped;
    pthread_mutex_unlock(&log_lock);
    if (dropped > 0)
        fprintf(f, "[logger] dropped %lu messages (queue full)\n",
                (unsigned long)dropped);

    fclose(f);
    return NULL;
}

/* --- public API --- */

int logger_init(void)
{
    if (pthread_create(&log_tid, NULL, logger_thread, NULL) != 0) {
        perror("pthread_create logger");
        return -1;
    }
    return 0;
}

void log_push(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    pthread_mutex_lock(&log_lock);

    if (log_count < LOG_QUEUE_MAX) {
        strncpy(log_queue[log_tail].msg, buf, 511);
        log_queue[log_tail].msg[511] = '\0';
        log_tail = (log_tail + 1) % LOG_QUEUE_MAX;
        log_count++;
        pthread_cond_signal(&log_cond);
    } else {
        log_dropped++;
    }

    pthread_mutex_unlock(&log_lock);
}

void logger_shutdown(void)
{
    pthread_mutex_lock(&log_lock);
    log_shutdown = 1;
    pthread_cond_signal(&log_cond);
    pthread_mutex_unlock(&log_lock);
}

void logger_join(void)
{
    pthread_join(log_tid, NULL);
}