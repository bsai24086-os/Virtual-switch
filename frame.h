#ifndef FRAME_H
#define FRAME_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>   /* ssize_t */

/* Total header size in bytes */
#define FRAME_HEADER_SIZE 17

/* Max payload size (spec says 0 to 1500) */
#define FRAME_MAX_PAYLOAD 1500

/* Destination MAC for broadcast */
#define MAC_BROADCAST "\xff\xff\xff\xff\xff\xff"

/*
 * The frame header as it arrives on the wire.
 * Fields are in network byte order (big-endian).
 * We use __attribute__((packed)) so the compiler adds no padding.
 */
typedef struct __attribute__((packed)) {
    uint16_t length;    /* payload length in bytes */
    uint8_t  src[6];    /* source MAC address */
    uint8_t  dst[6];    /* destination MAC address */
    uint16_t vlan;      /* VLAN number 1-4094 */
    uint8_t  priority;  /* 0=high, 1=normal, 2=low */
} FrameHeader;

/*
 * A full frame: header + payload buffer.
 * payload points to a malloc'd buffer of header.length bytes.
 */
typedef struct {
    FrameHeader header;
    uint8_t    *payload;
} Frame;

/*
 * Read exactly 'n' bytes from fd into buf.
 * Returns n on success.
 * Returns 0 if the connection closed cleanly.
 * Returns -1 on error.
 */
ssize_t read_exact(int fd, void *buf, size_t n);

/*
 * Write exactly 'n' bytes from buf into fd.
 * Returns n on success.
 * Returns -1 on error (including EPIPE).
 */
ssize_t write_exact(int fd, const void *buf, size_t n);

/*
 * Read one full frame (header + payload) from fd.
 * Caller must free frame->payload when done.
 * Returns 1 on success, 0 on clean close, -1 on error.
 */
int frame_read(int fd, Frame *frame);

/*
 * Write one full frame (header + payload) to fd.
 * Returns 1 on success, -1 on error.
 */
int frame_write(int fd, const Frame *frame);

/*
 * Print a frame to stdout for debugging.
 */
void frame_print(const Frame *frame);

#endif /* FRAME_H */