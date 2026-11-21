#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

#include "frame.h"

/*
 * Test: build a frame, write it into a pipe, read it back out,
 * verify every field survived intact.
 * A pipe is a file descriptor pair — perfect for testing I/O code
 * without needing a real network connection.
 */
int main(void)
{
    int      pipefd[2];
    Frame    out, in;
    char    *msg = "hello switch";
    uint16_t msg_len = (uint16_t)strlen(msg);

    /* --- build the outgoing frame --- */
    out.header.length   = htons(msg_len);
    out.header.src[0]   = 0xAA;
    out.header.src[1]   = 0xBB;
    out.header.src[2]   = 0xCC;
    out.header.src[3]   = 0x00;
    out.header.src[4]   = 0x00;
    out.header.src[5]   = 0x01;
    memcpy(out.header.dst, MAC_BROADCAST, 6);
    out.header.vlan     = htons(1);
    out.header.priority = 1;
    out.payload         = (uint8_t *)msg;

    /* --- create a pipe to simulate a socket --- */
    if (pipe(pipefd) < 0) {
        perror("pipe");
        return 1;
    }

    /* --- write the frame into the pipe --- */
    if (frame_write(pipefd[1], &out) != 1) {
        fprintf(stderr, "FAIL: frame_write\n");
        return 1;
    }
    close(pipefd[1]); /* signal EOF after writing */

    /* --- read the frame back out --- */
    in.payload = NULL;
    int r = frame_read(pipefd[0], &in);
    if (r != 1) {
        fprintf(stderr, "FAIL: frame_read returned %d\n", r);
        return 1;
    }
    close(pipefd[0]);

    /* --- verify every field --- */
    int failed = 0;

    if (ntohs(in.header.length) != msg_len) {
        fprintf(stderr, "FAIL: length %u != %u\n",
                ntohs(in.header.length), msg_len);
        failed = 1;
    }
    if (memcmp(in.header.src, out.header.src, 6) != 0) {
        fprintf(stderr, "FAIL: src MAC mismatch\n");
        failed = 1;
    }
    if (memcmp(in.header.dst, out.header.dst, 6) != 0) {
        fprintf(stderr, "FAIL: dst MAC mismatch\n");
        failed = 1;
    }
    if (ntohs(in.header.vlan) != 1) {
        fprintf(stderr, "FAIL: vlan\n");
        failed = 1;
    }
    if (in.header.priority != 1) {
        fprintf(stderr, "FAIL: priority\n");
        failed = 1;
    }
    if (memcmp(in.payload, msg, msg_len) != 0) {
        fprintf(stderr, "FAIL: payload mismatch\n");
        failed = 1;
    }

    if (!failed) {
        printf("PASS: frame round-trip OK\n");
        frame_print(&in);
    }

    free(in.payload);
    return failed;
}