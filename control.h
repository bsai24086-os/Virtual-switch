#define _POSIX_C_SOURCE 200809L

#ifndef CONTROL_H
#define CONTROL_H

#include "shm_stats.h"
#include "mac_table.h"

#define CTRL_SOCK_PATH "/tmp/vswitch.sock"
#define CTRL_BUF_SIZE  256

typedef struct {
    ShmStats    *stats;
    MacTable    *mac_table;
    int          num_ports;
} ControlContext;

/* What "show ports" prints besides the counters. */
typedef struct {
    int  vlan;
    int  trunk;
    char state[8];      /* "up", "free", "down" or "closing" */
} PortInfo;

/* Creates the UNIX socket and starts the control thread. 0 or -1. */
int  control_start(ControlContext *ctx);

/* Wakes the control thread, joins it, closes and unlinks the socket. */
void control_stop(void);

/* The functions below are implemented in switch.c. */
void port_set_mirror(int src, int dst);
int  port_admin_down(int port);          /* 0 ok, -1 bad port, -2 timeout */
int  port_admin_up(int port);            /* 0 ok, -1 bad port             */
int  port_set_vlan(int port, int vlan);  /* 0 ok, -1 bad port or vlan     */
int  port_set_trunk(int port, int on);   /* 0 ok, -1 bad port             */
void port_get_info(int port, PortInfo *out);

#endif /* CONTROL_H */