#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "shm_stats.h"
#include "control.h"
#include "instance.h"

/* --- send a command over the UNIX socket and print the response --- */
static int send_command(const char *cmd)
{
    int                fd;
    struct sockaddr_un addr;
    char               buf[CTRL_BUF_SIZE * 8];
    ssize_t            n;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    char pbuf[100];
    strncpy(addr.sun_path, inst_name(CTRL_SOCK_PATH, pbuf, sizeof(pbuf)), sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect — is the switch running?");
        close(fd);
        return -1;
    }

    /* send the command plus a newline */
    char   line[CTRL_BUF_SIZE + 2];
    int    len = snprintf(line, sizeof(line), "%s\n", cmd);
    size_t off = 0;
    while (off < (size_t)len) {
        ssize_t w = write(fd, line + off, (size_t)len - off);
        if (w < 0) {
            perror("write");
            close(fd);
            return -1;
        }
        off += (size_t)w;
    }

    /* read and print the response */
    while ((n = read(fd, buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';
        printf("%s", buf);
    }

    close(fd);
    return 0;
}

/* --- stats --watch: read shared memory directly --- */
static void stats_watch(void)
{
    ShmStats *stats = shm_stats_open();
    if (!stats) {
        fprintf(stderr, "cannot open shared memory — is the switch running?\n");
        return;
    }

    printf("watching stats (Ctrl+C to stop)...\n\n");

    while (1) {
        /* clear screen */
        printf("\033[2J\033[H");
        printf("PORT  FRAMES_IN  FRAMES_OUT  DROPPED  MIRRORED\n");
        printf("------------------------------------------------\n");

        pthread_mutex_lock(&stats->lock);
        for (int i = 0; i < stats->num_ports; i++) {
            PortCounters *p = &stats->ports[i];
            printf("%-5d %-10lu %-11lu %-8lu %-8lu\n",
                   i,
                   (unsigned long)p->frames_in,
                   (unsigned long)p->frames_out,
                   (unsigned long)p->frames_dropped,
                   (unsigned long)p->frames_mirrored);
        }
        pthread_mutex_unlock(&stats->lock);
        fflush(stdout);          /* show the table even when output is piped */

        sleep(1);
    }

    shm_stats_close(stats);
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr,
            "usage: ./swctl <command>\n"
            "commands:\n"
            "  show ports\n"
            "  show mac\n"
            "  clear mac\n"
            "  stats\n"
            "  stats --watch\n"
            "  mirror <src_port> <sniffer_port|off>\n"
            "  shutdown <port>\n"
            "  up <port>\n"
            "  vlan <port> <vlan>\n"
            "  trunk <port> <on|off>\n");
        return 1;
    }

    /* build the full command string from all argv */
    char   cmd[CTRL_BUF_SIZE] = {0};
    size_t used = 0;
    for (int i = 1; i < argc; i++) {
        int n = snprintf(cmd + used, sizeof(cmd) - used, "%s%s",
                         i > 1 ? " " : "", argv[i]);
        if (n < 0 || (size_t)n >= sizeof(cmd) - used) {
            fprintf(stderr, "command too long\n");
            return 1;
        }
        used += (size_t)n;
    }

    /* stats --watch goes direct to shared memory */
    if (strcmp(cmd, "stats --watch") == 0) {
        stats_watch();
        return 0;
    }

    /* everything else goes over the UNIX socket */
    return send_command(cmd);
}