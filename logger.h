#define _POSIX_C_SOURCE 200809L

#ifndef LOGGER_H
#define LOGGER_H

#define LOG_FILE      "/tmp/vswitch.log"
#define LOG_QUEUE_MAX 512

/*
 * Initialize the logger and start the logger thread.
 * Call once at startup before any log_push calls.
 */
int  logger_init(void);   /* 0 on success, -1 on error */

/*
 * Push a log message. Never blocks — drops if queue is full.
 * Accepts printf-style format string.
 */
void log_push(const char *fmt, ...);

/*
 * Signal the logger thread to flush and exit.
 * Call during shutdown before joining.
 */
void logger_shutdown(void);

/*
 * Join the logger thread. Call after logger_shutdown().
 */
void logger_join(void);

#endif /* LOGGER_H */