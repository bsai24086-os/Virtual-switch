#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>   /* htons, ntohs */
#include <errno.h>

#include "frame.h"

/* --- low-level reliable I/O --- */

ssize_t read_exact(int fd, void *buf, size_t n)
{
    size_t  total = 0;
    ssize_t got;
    uint8_t *ptr = (uint8_t *)buf;

    while (total < n) {
        got = read(fd, ptr + total, n - total);

        if (got == 0)
            return 0;        /* connection closed cleanly */

        if (got < 0) {
            if (errno == EINTR)
                continue;    /* interrupted by signal, retry */
            return -1;       /* real error */
        }

        total += (size_t)got;
    }

    return (ssize_t)total;
}

ssize_t write_exact(int fd, const void *buf, size_t n)
{
    size_t        total = 0;
    ssize_t       sent;
    const uint8_t *ptr = (const uint8_t *)buf;

    while (total < n) {
        sent = write(fd, ptr + total, n - total);

        if (sent < 0) {
            if (errno == EINTR)
                continue;    /* interrupted by signal, retry */
            /* EPIPE means the other end closed — caller handles it */
            return -1;
        }

        total += (size_t)sent;
    }

    return (ssize_t)total;
}

/* --- frame-level I/O --- */

int frame_read(int fd, Frame *frame)
{
    ssize_t  r;
    uint16_t payload_len;

    /* Step 1: read the fixed-size header */
    r = read_exact(fd, &frame->header, FRAME_HEADER_SIZE);
    if (r == 0) return 0;   /* clean disconnect */
    if (r < 0)  return -1;  /* error */

    /* Step 2: convert length from network byte order */
    payload_len = ntohs(frame->header.length);

    /* Step 3: validate length */
    if (payload_len > FRAME_MAX_PAYLOAD) {
        fprintf(stderr, "frame_read: payload too large (%u)\n", payload_len);
        return -1;
    }

    /* Step 4: allocate payload buffer */
    frame->payload = NULL;
    if (payload_len > 0) {
        frame->payload = malloc(payload_len);
        if (!frame->payload) {
            perror("malloc");
            return -1;
        }

        /* Step 5: read the payload */
        r = read_exact(fd, frame->payload, payload_len);
        if (r == 0) {
            /* closed mid-frame — treat as error */
            free(frame->payload);
            frame->payload = NULL;
            return -1;
        }
        if (r < 0) {
            free(frame->payload);
            frame->payload = NULL;
            return -1;
        }
    }

    return 1; /* success */
}

int frame_write(int fd, const Frame *frame)
{
    ssize_t  w;
    uint16_t payload_len = ntohs(frame->header.length);

    /* Write header first */
    w = write_exact(fd, &frame->header, FRAME_HEADER_SIZE);
    if (w < 0) return -1;

    /* Write payload if there is one */
    if (payload_len > 0 && frame->payload != NULL) {
        w = write_exact(fd, frame->payload, payload_len);
        if (w < 0) return -1;
    }

    return 1;
}

/* --- debug helper --- */

void frame_print(const Frame *frame)
{
    uint16_t len  = ntohs(frame->header.length);
    uint16_t vlan = ntohs(frame->header.vlan);
    const uint8_t *s = frame->header.src;
    const uint8_t *d = frame->header.dst;

    printf("Frame: src=%02x:%02x:%02x:%02x:%02x:%02x"
           "  dst=%02x:%02x:%02x:%02x:%02x:%02x"
           "  vlan=%u  pri=%u  len=%u\n",
           s[0], s[1], s[2], s[3], s[4], s[5],
           d[0], d[1], d[2], d[3], d[4], d[5],
           vlan, frame->header.priority, len);

    if (len > 0 && frame->payload != NULL)
        printf("  payload: \"%.*s\"\n", (int)len, (char *)frame->payload);
}