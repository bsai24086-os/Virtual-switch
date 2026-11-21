#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>

#include "shm_stats.h"

/*
 * Test: parent creates shared memory and increments counters.
 * Child opens the same shared memory and reads the counters.
 * Both should see the same numbers.
 */
int main(void)
{
    ShmStats *stats;
    pid_t     pid;

    /* parent creates the shared memory */
    stats = shm_stats_create(4);
    if (!stats) {
        fprintf(stderr, "FAIL: shm_stats_create\n");
        return 1;
    }

    /* parent increments some counters */
    shm_inc_in(stats,      0);
    shm_inc_in(stats,      0);
    shm_inc_in(stats,      0);  /* port 0 frames_in = 3 */
    shm_inc_out(stats,     1);  /* port 1 frames_out = 1 */
    shm_inc_dropped(stats, 2);  /* port 2 frames_dropped = 1 */
    shm_inc_mirrored(stats,3);  /* port 3 frames_mirrored = 1 */

    /* fork a child to read the same shared memory */
    fflush(stdout);   /* or the child reprints our buffered output */
    pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }

    if (pid == 0) {
        /* --- child process --- */
        ShmStats *child_view = shm_stats_open();
        if (!child_view) {
            fprintf(stderr, "FAIL: child shm_stats_open\n");
            exit(1);
        }

        int failed = 0;

        if (child_view->ports[0].frames_in != 3) {
            fprintf(stderr, "FAIL: port0 frames_in = %lu, want 3\n",
                    child_view->ports[0].frames_in);
            failed = 1;
        }
        if (child_view->ports[1].frames_out != 1) {
            fprintf(stderr, "FAIL: port1 frames_out = %lu, want 1\n",
                    child_view->ports[1].frames_out);
            failed = 1;
        }
        if (child_view->ports[2].frames_dropped != 1) {
            fprintf(stderr, "FAIL: port2 frames_dropped = %lu, want 1\n",
                    child_view->ports[2].frames_dropped);
            failed = 1;
        }
        if (child_view->ports[3].frames_mirrored != 1) {
            fprintf(stderr, "FAIL: port3 frames_mirrored = %lu, want 1\n",
                    child_view->ports[3].frames_mirrored);
            failed = 1;
        }

        if (!failed)
            printf("PASS: child sees all counters correctly\n");

        shm_stats_close(child_view);
        exit(failed);
    }

    /* --- parent waits for child --- */
    int status;
    waitpid(pid, &status, 0);

    shm_stats_close(stats);
    shm_stats_unlink();

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
        printf("PASS: shared memory round-trip OK\n");
    else
        printf("FAIL: child exited with error\n");

    return 0;
}