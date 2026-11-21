#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <time.h>
#include <pthread.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "control.h"
#include "shm_stats.h"
#include "mac_table.h"
#include "logger.h"

/* Set in control_start before the thread exists, so no lock is needed. */
static int            g_server_fd   = -1;
static int            g_wake[2]     = {-1, -1};   /* self-pipe to stop us */
static pthread_t      g_ctrl_tid;
static int            g_ctrl_running = 0;
static ControlContext g_ctx;

/* ------------------------------------------------------------------ */
/*  Bounded append into the reply buffer                                */
/* ------------------------------------------------------------------ */

typedef struct {
    char   buf[CTRL_BUF_SIZE * 16];
    size_t len;
} Reply;

static void reply_add(Reply *r, const char *fmt, ...)
{
    if (r->len >= sizeof(r->buf) - 1) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(r->buf + r->len, sizeof(r->buf) - r->len, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    r->len += (size_t)n;
    if (r->len > sizeof(r->buf) - 1) r->len = sizeof(r->buf) - 1;
}
/* with_info = 1 adds VLAN, MODE and STATE columns at the end. */
static void reply_ports(Reply *r, int with_info)
{
    reply_add(r, "PORT  FRAMES_IN  FRAMES_OUT  DROPPED  MIRRORED");
    reply_add(r, with_info ? "  VLAN  MODE   STATE\n" : "\n");

    for (int i = 0; i < g_ctx.num_ports; i++) {
        PortInfo info;
        port_get_info(i, &info);             /* before the stats lock */

        pthread_mutex_lock(&g_ctx.stats->lock);
        PortCounters p = g_ctx.stats->ports[i];
        pthread_mutex_unlock(&g_ctx.stats->lock);

        reply_add(r, "%-5d %-10lu %-11lu %-8lu %-8lu", i,
                  (unsigned long)p.frames_in,
                  (unsigned long)p.frames_out,
                  (unsigned long)p.frames_dropped,
                  (unsigned long)p.frames_mirrored);
        if (with_info)
            reply_add(r, " %-5d %-6s %s", info.vlan,
                      info.trunk ? "trunk" : "access", info.state);
        reply_add(r, "\n");
    }
}

/* Parses a whole decimal number in lo..hi. 0 on success. */
static int parse_num(const char *s, long lo, long hi, int *out)
{
    char *end;
    long  v = strtol(s, &end, 10);
    if (s[0] == '\0' || *end != '\0' || v < lo || v > hi) return -1;
    *out = (int)v;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Handle one swctl connection                                         */
/* ------------------------------------------------------------------ */
static void handle_client(int client_fd)
{
    char    cmd[CTRL_BUF_SIZE];
    Reply   r;
    ssize_t n;

    r.len = 0;
    r.buf[0] = '\0';

    n = read(client_fd, cmd, sizeof(cmd) - 1);
    if (n <= 0) return;
    cmd[n] = '\0';

    size_t len = strlen(cmd);
    while (len > 0 && (cmd[len - 1] == '\n' || cmd[len - 1] == '\r'))
        cmd[--len] = '\0';

    log_push("control: command \"%s\"", cmd);

    if (strcmp(cmd, "show ports") == 0) {
        reply_ports(&r, 1);

    } else if (strcmp(cmd, "stats") == 0) {
        reply_ports(&r, 0);

    } else if (strcmp(cmd, "show mac") == 0) {
        static MacSnap snap[MAC_TABLE_MAX];   /* only the control thread uses it */
        int cnt = mac_table_snapshot(g_ctx.mac_table, snap, MAC_TABLE_MAX);
        reply_add(&r, "%-20s %-6s %s\n", "MAC", "PORT", "AGE(s)");
        for (int i = 0; i < cnt; i++)
            reply_add(&r, "%02x:%02x:%02x:%02x:%02x:%02x    %-6d %ld\n",
                      snap[i].mac[0], snap[i].mac[1], snap[i].mac[2],
                      snap[i].mac[3], snap[i].mac[4], snap[i].mac[5],
                      snap[i].port_idx, snap[i].age);

    } else if (strcmp(cmd, "clear mac") == 0) {
        mac_table_clear(g_ctx.mac_table);
        reply_add(&r, "MAC table cleared\n");

    } else if (strncmp(cmd, "mirror ", 7) == 0) {
        int  src = -1;
        char dst_str[16] = {0};
        if (sscanf(cmd + 7, "%d %15s", &src, dst_str) != 2) {
            reply_add(&r, "usage: mirror <src> <dst|off>\n");
        } else if (src < 0 || src >= g_ctx.num_ports) {
            reply_add(&r, "invalid port %d\n", src);
        } else if (strcmp(dst_str, "off") == 0) {
            port_set_mirror(src, -1);
            reply_add(&r, "port %d mirroring disabled\n", src);
        } else {
            char *end;
            long  dst = strtol(dst_str, &end, 10);
            if (*end != '\0' || dst < 0 || dst >= g_ctx.num_ports || dst == src) {
                reply_add(&r, "invalid sniffer port %s\n", dst_str);
            } else {
                port_set_mirror(src, (int)dst);
                reply_add(&r, "port %d now mirroring to port %ld\n", src, dst);
            }
        }

    } else if (strncmp(cmd, "shutdown ", 9) == 0 || strncmp(cmd, "up ", 3) == 0) {
        int  down = (cmd[0] == 's');
        int  port;
        if (parse_num(cmd + (down ? 9 : 3), 0, g_ctx.num_ports - 1, &port) < 0) {
            reply_add(&r, "invalid port\n");
        } else if (down) {
            int rc = port_admin_down(port);
            if (rc == 0) reply_add(&r, "port %d shut down\n", port);
            else         reply_add(&r, "port %d is closing, try again\n", port);
        } else {
            port_admin_up(port);
            reply_add(&r, "port %d up\n", port);
        }

    } else if (strncmp(cmd, "vlan ", 5) == 0) {
        int  port, vlan;
        char a[16] = {0}, b[16] = {0};
        if (sscanf(cmd + 5, "%15s %15s", a, b) != 2 ||
            parse_num(a, 0, g_ctx.num_ports - 1, &port) < 0) {
            reply_add(&r, "usage: vlan <port> <1-4094>\n");
        } else if (parse_num(b, 1, 4094, &vlan) < 0) {
            reply_add(&r, "invalid VLAN %s (1-4094)\n", b);
        } else {
            port_set_vlan(port, vlan);
            reply_add(&r, "port %d is now in VLAN %d\n", port, vlan);
        }

    } else if (strncmp(cmd, "trunk ", 6) == 0) {
        int  port;
        char a[16] = {0}, b[16] = {0};
        if (sscanf(cmd + 6, "%15s %15s", a, b) != 2 ||
            parse_num(a, 0, g_ctx.num_ports - 1, &port) < 0 ||
            (strcmp(b, "on") != 0 && strcmp(b, "off") != 0)) {
            reply_add(&r, "usage: trunk <port> <on|off>\n");
        } else {
            port_set_trunk(port, strcmp(b, "on") == 0);
            reply_add(&r, "port %d trunk %s\n", port, b);
        }

    } else {
        reply_add(&r, "unknown command: %s\n"
                      "commands: show ports, show mac, clear mac, stats, "
                      "shutdown <port>, up <port>, vlan <port> <vlan>, "
                      "trunk <port> <on|off>, mirror <src> <dst|off>\n", cmd);
    }

    size_t off = 0;
    while (off < r.len) {
        ssize_t w = write(client_fd, r.buf + off, r.len - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            break;                       /* swctl went away, fine */
        }
        off += (size_t)w;
    }
}
/* ------------------------------------------------------------------ */
/*  Control thread: poll the listening socket and the wake-up pipe      */
/* ------------------------------------------------------------------ */

static void *control_thread(void *arg)
{
    (void)arg;
    for (;;) {
        struct pollfd pf[2] = {
            { .fd = g_server_fd, .events = POLLIN },
            { .fd = g_wake[0],   .events = POLLIN },
        };
        if (poll(pf, 2, -1) < 0) {
            if (errno == EINTR) continue;
            perror("control poll");
            break;
        }
        if (pf[1].revents) break;                    /* control_stop() */
        if (!(pf[0].revents & POLLIN)) continue;

        int c = accept(g_server_fd, NULL, NULL);
        if (c < 0) {
            if (errno != EINTR && errno != ECONNABORTED)
                perror("control accept");
            continue;
        }
        /* a client that connects and says nothing cannot hang us */
        struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
        if (setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0)
            perror("control setsockopt");
        handle_client(c);
        close(c);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                          */
/* ------------------------------------------------------------------ */

int control_start(ControlContext *ctx)
{
    struct sockaddr_un addr;

    g_ctx = *ctx;

    unlink(CTRL_SOCK_PATH);                 /* stale socket from a crash */

    g_server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_server_fd < 0) { perror("control socket"); return -1; }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CTRL_SOCK_PATH, sizeof(addr.sun_path) - 1);

    if (bind(g_server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("control bind");
        goto fail;
    }
    if (listen(g_server_fd, 5) < 0) {
        perror("control listen");
        goto fail;
    }
    if (pipe(g_wake) < 0) {
        perror("control pipe");
        goto fail;
    }
    if (pthread_create(&g_ctrl_tid, NULL, control_thread, NULL) != 0) {
        perror("pthread_create control");
        close(g_wake[0]);
        close(g_wake[1]);
        goto fail;
    }
    g_ctrl_running = 1;
    printf("[control] listening on %s\n", CTRL_SOCK_PATH);
    return 0;

fail:
    close(g_server_fd);
    g_server_fd = -1;
    unlink(CTRL_SOCK_PATH);
    return -1;
}

void control_stop(void)
{
    if (!g_ctrl_running) return;

    if (write(g_wake[1], "x", 1) < 0)
        perror("control wake");
    pthread_join(g_ctrl_tid, NULL);
    g_ctrl_running = 0;

    close(g_wake[0]);
    close(g_wake[1]);
    close(g_server_fd);
    g_server_fd = -1;
    unlink(CTRL_SOCK_PATH);
    printf("[control] stopped, socket removed\n");
}
