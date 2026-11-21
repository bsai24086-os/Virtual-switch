#define _POSIX_C_SOURCE 200809L

/*
 * Supervisor: runs ./switch and restarts it if it crashes.
 *
 *   ./supervisor <tcp_port> <num_ports> [config_file]
 *
 * The switch keeps its MAC table in POSIX shared memory. Under the
 * supervisor it is started with VSWITCH_PERSIST=1, so after a crash the
 * new process attaches to the old table and keeps the learned MACs.
 * A clean exit of the switch (exit code 0, for example after SIGINT or
 * SIGTERM) ends the supervisor too.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

#define MAX_RESTARTS_IN_WINDOW 5     /* more crashes than this ...        */
#define RESTART_WINDOW_SECS    10    /* ... within this many seconds = stop */

static volatile sig_atomic_t g_child = 0;
static volatile sig_atomic_t g_stop  = 0;

/* Forward SIGINT/SIGTERM to the switch and remember that we are stopping. */
static void on_signal(int sig)
{
    g_stop = 1;
    if (g_child > 0)
        kill((pid_t)g_child, sig);
}

static pid_t start_switch(char *const args[])
{
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return -1;
    }
    if (pid == 0) {
        signal(SIGINT,  SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        execv(args[0], args);
        perror("execv ./switch");
        _exit(127);
    }
    return pid;
}

int main(int argc, char *argv[])
{
    if (argc < 3 || argc > 4) {
        fprintf(stderr, "usage: ./supervisor <tcp_port> <num_ports> [config_file]\n");
        return 1;
    }

    /* the switch is started as: ./switch <args of the supervisor> */
    char *args[6] = { "./switch", argv[1], argv[2], NULL, NULL, NULL };
    if (argc == 4) args[3] = argv[3];

    if (setenv("VSWITCH_PERSIST", "1", 1) != 0) {
        perror("setenv");
        return 1;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);              /* no SA_RESTART: waitpid returns EINTR */
    if (sigaction(SIGINT, &sa, NULL) != 0 || sigaction(SIGTERM, &sa, NULL) != 0) {
        perror("sigaction");
        return 1;
    }

    time_t crash_times[MAX_RESTARTS_IN_WINDOW + 1];
    int    crashes = 0;

    for (;;) {
        pid_t pid = start_switch(args);
        if (pid < 0) return 1;
        g_child = pid;
        if (g_stop) kill(pid, SIGTERM);    /* a signal came in during fork */
        printf("[supervisor] switch started, pid %d\n", (int)pid);
        fflush(stdout);

        int status = 0;
        pid_t w;
        do {
            w = waitpid(pid, &status, 0);
        } while (w < 0 && errno == EINTR);
        g_child = 0;

        if (w < 0) {
            perror("waitpid");
            return 1;
        }

        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            printf("[supervisor] switch exited cleanly, stopping\n");
            return 0;
        }

        if (WIFSIGNALED(status))
            printf("[supervisor] switch killed by signal %d\n", WTERMSIG(status));
        else
            printf("[supervisor] switch exited with code %d\n", WEXITSTATUS(status));
        fflush(stdout);

        if (g_stop) {
            printf("[supervisor] stop requested, not restarting\n");
            return 0;
        }

        /* give up if it keeps crashing right away */
        time_t now = time(NULL);
        int keep = 0;
        for (int i = 0; i < crashes; i++)
            if (now - crash_times[i] < RESTART_WINDOW_SECS)
                crash_times[keep++] = crash_times[i];
        crashes = keep;
        if (crashes >= MAX_RESTARTS_IN_WINDOW) {
            fprintf(stderr, "[supervisor] %d crashes in %d s, giving up\n",
                    MAX_RESTARTS_IN_WINDOW, RESTART_WINDOW_SECS);
            return 1;
        }
        crash_times[crashes++] = now;

        printf("[supervisor] restarting switch\n");
        fflush(stdout);
    }
}