/*
 * twoswitch.c - Tier 3: two switch processes joined by a trunk.
 *
 *   ./twoswitch <tcp_port_a> <tcp_port_b> <num_ports> [switch_binary]
 *
 * Like two_way.c from Lab 05: two pipes make one two-way channel.
 *   ab: switch A writes, switch B reads
 *   ba: switch B writes, switch A reads
 * Each child gets its two ends in VSWITCH_TRUNK="read_fd,write_fd" and a
 * name in VSWITCH_NAME (a or b) so the two switches do not share a
 * control socket or shared memory. The output of each switch comes back
 * through a third pipe and is printed with an "A| " or "B| " prefix.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

static void close_fd(int fd)
{
    if (fd >= 0 && close(fd) < 0) perror("close");
}

/* Child: keep only my two trunk ends and my output pipe, then exec. */
static void run_child(const char *name, const char *bin, const char *port,
                      const char *nports, int rfd, int wfd, int out_w,
                      const int *all, int nall)
{
    for (int i = 0; i < nall; i++)
        if (all[i] != rfd && all[i] != wfd && all[i] != out_w) close_fd(all[i]);

    if (dup2(out_w, STDOUT_FILENO) < 0 || dup2(out_w, STDERR_FILENO) < 0) {
        perror("dup2");
        _exit(1);
    }
    close_fd(out_w);

    char trunk[32];
    snprintf(trunk, sizeof(trunk), "%d,%d", rfd, wfd);
    if (setenv("VSWITCH_TRUNK", trunk, 1) < 0 || setenv("VSWITCH_NAME", name, 1) < 0) {
        perror("setenv");
        _exit(1);
    }
    char *args[] = { (char *)bin, (char *)port, (char *)nports, NULL };
    execv(bin, args);
    perror("execv");
    _exit(1);
}

/* Print complete lines from buf with a prefix; keep the rest. */
static void flush_lines(const char *tag, char *buf, size_t *len)
{
    size_t start = 0;
    for (size_t i = 0; i < *len; i++) {
        if (buf[i] == '\n') {
            printf("%s| %.*s\n", tag, (int)(i - start), buf + start);
            start = i + 1;
        }
    }
    memmove(buf, buf + start, *len - start);
    *len -= start;
}

int main(int argc, char *argv[])
{
    if (argc < 4 || argc > 5) {
        fprintf(stderr, "usage: %s <tcp_port_a> <tcp_port_b> <num_ports> [switch_binary]\n",
                argv[0]);
        return 1;
    }
    const char *bin = argc == 5 ? argv[4] : "./switch";
    setvbuf(stdout, NULL, _IOLBF, 0);

    int ab[2], ba[2], outa[2], outb[2];
    if (pipe(ab) < 0 || pipe(ba) < 0 || pipe(outa) < 0 || pipe(outb) < 0) {
        perror("pipe");
        return 1;
    }
    int all[8] = { ab[0], ab[1], ba[0], ba[1], outa[0], outa[1], outb[0], outb[1] };

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;            /* no SA_RESTART: poll() wakes up */
    if (sigemptyset(&sa.sa_mask) < 0 ||
        sigaction(SIGINT, &sa, NULL) < 0 || sigaction(SIGTERM, &sa, NULL) < 0) {
        perror("sigaction");
        return 1;
    }

    pid_t pa = fork();
    if (pa < 0) { perror("fork"); return 1; }
    if (pa == 0) run_child("a", bin, argv[1], argv[3], ba[0], ab[1], outa[1], all, 8);

    pid_t pb = fork();
    if (pb < 0) { perror("fork"); kill(pa, SIGTERM); return 1; }
    if (pb == 0) run_child("b", bin, argv[2], argv[3], ab[0], ba[1], outb[1], all, 8);

    /* parent: the trunk belongs to the children now */
    for (int i = 0; i < 8; i++)
        if (all[i] != outa[0] && all[i] != outb[0]) close_fd(all[i]);

    printf("twoswitch: A pid %d on TCP %s, B pid %d on TCP %s\n",
           (int)pa, argv[1], (int)pb, argv[2]);
    printf("twoswitch: use VSWITCH_NAME=a ./swctl ... or VSWITCH_NAME=b ./swctl ...\n");

    char  bufa[8192], bufb[8192];
    size_t la = 0, lb = 0;
    int   open_a = 1, open_b = 1, forwarded = 0;

    while (open_a || open_b) {
        if (g_stop && !forwarded) {       /* pass Ctrl-C / kill on to both */
            kill(pa, SIGTERM);
            kill(pb, SIGTERM);
            forwarded = 1;
        }
        struct pollfd pf[2] = {
            { .fd = open_a ? outa[0] : -1, .events = POLLIN },
            { .fd = open_b ? outb[0] : -1, .events = POLLIN },
        };
        if (poll(pf, 2, -1) < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            break;
        }
        for (int k = 0; k < 2; k++) {
            if (!pf[k].revents) continue;
            char   *buf = k == 0 ? bufa : bufb;
            size_t *len = k == 0 ? &la : &lb;
            ssize_t n = read(pf[k].fd, buf + *len, sizeof(bufa) - 1 - *len);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) {                 /* that switch has exited */
                if (*len > 0) { buf[(*len)++] = '\n'; flush_lines(k ? "B" : "A", buf, len); }
                if (n < 0) perror("read");
                close_fd(pf[k].fd);
                if (k == 0) open_a = 0; else open_b = 0;
                continue;
            }
            *len += (size_t)n;
            if (*len >= sizeof(bufa) - 1) { buf[(*len)++] = '\n'; }   /* very long line */
            flush_lines(k ? "B" : "A", buf, len);
        }
    }

    int rc = 0;
    pid_t pids[2] = { pa, pb };
    for (int k = 0; k < 2; k++) {
        int st;
        while (waitpid(pids[k], &st, 0) < 0) {
            if (errno != EINTR) { perror("waitpid"); rc = 1; break; }
        }
        if (WIFEXITED(st))
            printf("twoswitch: switch %c exited with %d\n", k ? 'B' : 'A', WEXITSTATUS(st));
        else if (WIFSIGNALED(st))
            printf("twoswitch: switch %c killed by signal %d\n", k ? 'B' : 'A', WTERMSIG(st));
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) rc = 1;
    }
    return rc;
}