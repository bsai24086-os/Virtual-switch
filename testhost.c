#define _POSIX_C_SOURCE 200809L
/*
 * testhost: scripted host for the test suite.
 *
 *   testhost <tcp_port> <mac> send  <dst|bc> <count> <prio> [gap_us]
 *   testhost <tcp_port> <mac> mix   <dst> <n_each>     H,N,L interleaved
 *   testhost <tcp_port> <mac> recv  <seconds>          count what arrives
 *   testhost <tcp_port> <mac> stall <seconds>          never reads
 *   testhost <tcp_port> <mac> hello                    one frame, then wait 1s
 *
 * <mac> and <dst> are the last byte of 02:00:00:00:00:XX. Every mode first
 * sends one broadcast "hello" frame so the switch learns the MAC.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "frame.h"

static uint8_t my_mac[6] = {0x02, 0, 0, 0, 0, 0};

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

static int frame_vlan = 1;   /* VLAN env: vlan field of frames we send */
static int pad_to = 0;     /* PAYLOAD_SIZE env: pad data frames */

static int send_one(int fd, int dst, int prio, const char *msg)
{
    Frame f;
    static char big[FRAME_MAX_PAYLOAD + 1];
    uint16_t len = (uint16_t)strlen(msg);
    if (pad_to > len && strcmp(msg, "hello") != 0) {
        memset(big, '.', (size_t)pad_to);
        memcpy(big, msg, len);
        big[pad_to] = '\0';
        msg = big;
        len = (uint16_t)pad_to;
    }
    f.header.length = htons(len);
    memcpy(f.header.src, my_mac, 6);
    if (dst < 0) {
        memset(f.header.dst, 0xff, 6);
    } else {
        uint8_t d[6] = {0x02, 0, 0, 0, 0, (uint8_t)dst};
        memcpy(f.header.dst, d, 6);
    }
    f.header.vlan     = htons((uint16_t)frame_vlan);
    f.header.priority = (uint8_t)prio;
    f.payload         = (uint8_t *)msg;
    return frame_write(fd, &f);
}

static void pause_us(long us)
{
    struct timespec t = { us / 1000000, (us % 1000000) * 1000 };
    nanosleep(&t, NULL);
}

int main(int argc, char *argv[])
{
    if (argc < 4) {
        fprintf(stderr, "see the comment at the top of testhost.c\n");
        return 2;
    }
    int port = atoi(argv[1]);
    my_mac[5] = (uint8_t)atoi(argv[2]);
    const char *mode = argv[3];

    signal(SIGPIPE, SIG_IGN);                 /* switch may close us */
    const char *vs = getenv("VLAN");
    if (vs) frame_vlan = atoi(vs);
    const char *ps = getenv("PAYLOAD_SIZE");
    if (ps) pad_to = atoi(ps) > FRAME_MAX_PAYLOAD ? FRAME_MAX_PAYLOAD : atoi(ps);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }
    if (strcmp(mode, "stall") == 0) {        /* fill up fast */
        int small = 4096;
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &small, sizeof(small));
    }
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port   = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
        perror("connect");
        return 1;
    }

    if (strcmp(mode, "full") == 0) {          /* expect "switch full" */
        char buf[64] = {0};
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        printf("FULL_REPLY=%s", n > 0 ? buf : "(none)\n");
        close(fd);
        return 0;
    }

    if (send_one(fd, -1, 1, "hello") < 0) { perror("hello"); return 1; }
    const char *d = getenv("START_DELAY_MS");
    if (d) pause_us(atol(d) * 1000L);

    if (strcmp(mode, "hello") == 0) {
        sleep(1);

    } else if (strcmp(mode, "spoof") == 0 && argc >= 5) {
        int n = atoi(argv[4]);              /* n different source MACs */
        for (int i = 0; i < n; i++) {
            my_mac[4] = (uint8_t)(0x80 + i);
            if (send_one(fd, -1, 1, "spoof") < 0) { printf("SPOOF cut off at %d\n", i); break; }
            pause_us(20000);
        }
        char b[64];
        struct timeval tv = { 1, 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ssize_t r;
        while ((r = read(fd, b, sizeof(b))) > 0)
            ;
        printf("SPOOF_DONE closed_by_switch=%s\n", r == 0 ? "yes" : "no");

    } else if (strcmp(mode, "send") == 0 && argc >= 7) {
        int dst = strcmp(argv[4], "bc") == 0 ? -1 : atoi(argv[4]);
        int cnt = atoi(argv[5]), prio = atoi(argv[6]);
        long gap = argc >= 8 ? atol(argv[7]) : 0;
        pause_us(300000);                     /* let the receivers settle */
        double t0 = now_ms();
        for (int i = 0; i < cnt; i++) {
            char msg[32];
            snprintf(msg, sizeof(msg), "d%d %d", prio, i);
            if (send_one(fd, dst, prio, msg) < 0) { perror("send"); break; }
            if (gap) pause_us(gap);
        }
        printf("SENT=%d in %.0f ms\n", cnt, now_ms() - t0);
        pause_us(500000);

    } else if (strcmp(mode, "mix") == 0 && argc >= 6) {
        int dst = atoi(argv[4]), n = atoi(argv[5]);
        pause_us(300000);
        for (int i = 0; i < n; i++)
            for (int p = 0; p < 3; p++) {
                char msg[32];
                snprintf(msg, sizeof(msg), "d%d %d", p, i);
                if (send_one(fd, dst, p, msg) < 0) { perror("send"); return 1; }
            }
        printf("SENT=%d mixed\n", 3 * n);
        pause_us(500000);

    } else if (strcmp(mode, "recv") == 0 && argc >= 5) {
        double secs = atof(argv[4]);
        struct timeval tv = { 0, 200000 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        double end = now_ms() + secs * 1000, first = -1, last = -1;
        int cnt[3] = {0}, hello = 0, total = 0, lastvlan = 0;
        static char seq[4096];
        static double times[100000];
        while (now_ms() < end) {
            Frame f;
            int r = frame_read(fd, &f);
            if (r == 0) break;
            if (r < 0) continue;              /* timeout, keep waiting */
            uint16_t len = ntohs(f.header.length);
            if (len == 5 && memcmp(f.payload, "hello", 5) == 0) {
                hello++;
            } else {
                double t = now_ms();
                if (first < 0) first = t;
                last = t;
                lastvlan = ntohs(f.header.vlan);
                int p = f.header.priority > 2 ? 1 : f.header.priority;
                cnt[p]++;
                if (total < (int)sizeof(seq) - 1) seq[total] = "HNL"[p];
                if (total < 100000) times[total] = t - first;
                total++;
            }
            free(f.payload);
        }
        seq[total < (int)sizeof(seq) - 1 ? total : (int)sizeof(seq) - 1] = '\0';
        int window = 0;                       /* frames in [1000, 3000) ms */
        for (int i = 0; i < total && i < 100000; i++)
            if (times[i] >= 1000 && times[i] < 3000) window++;
        printf("RECV total=%d H=%d N=%d L=%d hello=%d lastvlan=%d span_ms=%.0f rate_1to3s=%.1f/s\n",
               total, cnt[0], cnt[1], cnt[2], hello, lastvlan,
               total ? last - first : 0.0, window / 2.0);
        printf("SEQ=%.90s\n", seq);

    } else if (strcmp(mode, "stall") == 0 && argc >= 5) {
        sleep((unsigned)atoi(argv[4]));       /* never read */

    } else {
        fprintf(stderr, "bad mode\n");
        return 2;
    }
    close(fd);
    return 0;
}