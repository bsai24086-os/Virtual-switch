#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "frame.h"

/*
 * Receiver thread — reads and prints any frames the switch forwards
 * to us (unicast or broadcast from other hosts).
 * Runs until the socket is closed.
 */
static void *recv_thread(void *arg)
{
    int   fd = *(int *)arg;
    Frame frame;

    while (1) {
        int r = frame_read(fd, &frame);
        if (r == 0) {
            printf("[host] switch closed connection\n");
            break;
        }
        if (r < 0) {
            printf("[host] recv error\n");
            break;
        }
        printf("[host] received frame: ");
        frame_print(&frame);
        free(frame.payload);
        frame.payload = NULL;
    }
    return NULL;
}

int main(int argc, char *argv[])
{
    /*
     * Usage:
     *   ./host <switch_ip> <tcp_port> <mac_last_byte>
     *       — connect, send 3 frames, disconnect  (original behaviour)
     *   ./host <switch_ip> <tcp_port> <mac_last_byte> loop
     *       — connect, send frames continuously until Ctrl+C
     */
    if (argc < 4 || argc > 5) {
        fprintf(stderr,
            "usage: ./host <switch_ip> <tcp_port> <mac_last_byte> [loop]\n"
            "  e.g. ./host 127.0.0.1 9000 1\n"
            "       ./host 127.0.0.1 9000 1 loop\n");
        return 1;
    }

    int loop_mode = (argc == 5 && strcmp(argv[4], "loop") == 0);

    uint8_t src_mac[6] = {0xAA, 0xBB, 0xCC, 0x00, 0x00, 0x01};
    src_mac[5] = (uint8_t)atoi(argv[3]);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)atoi(argv[2]));

    if (inet_pton(AF_INET, argv[1], &addr.sin_addr) <= 0) {
        perror("inet_pton"); return 1;
    }
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect"); return 1;
    }

    printf("connected to switch at %s:%s\n", argv[1], argv[2]);

    /* start receiver thread so we can see frames forwarded to us */
    pthread_t rtid;
    int fd_copy = fd;
    if (pthread_create(&rtid, NULL, recv_thread, &fd_copy) != 0) {
        perror("pthread_create recv");
        close(fd);
        return 1;
    }
    pthread_detach(rtid);

    /*
     * Brief wait so both hosts can connect before sending.
     * sleep() is acceptable in a test client (not the switch).
     */
    sleep(1);

    Frame  frame;
    int    frame_num = 1;
    int    max_frames = loop_mode ? 0 : 3;   /* 0 = unlimited */

    while (1) {
        char buf[64];
        snprintf(buf, sizeof(buf), "hello from host %d, frame %d",
                 src_mac[5], frame_num);

        uint16_t msg_len = (uint16_t)strlen(buf);

        frame.header.length   = htons(msg_len);
        memcpy(frame.header.src, src_mac, 6);
        memcpy(frame.header.dst, MAC_BROADCAST, 6);
        frame.header.vlan     = htons(1);
        frame.header.priority = 1;
        frame.payload         = (uint8_t *)buf;

        if (frame_write(fd, &frame) < 0) {
            perror("frame_write");
            break;
        }
        printf("sent frame %d: \"%s\"\n", frame_num, buf);

        if (!loop_mode && frame_num >= max_frames)
            break;

        frame_num++;

        if (loop_mode)
            sleep(1);   /* 1 frame/sec in loop mode — easy to follow */
    }

    close(fd);
    printf("host disconnected\n");
    return 0;
}