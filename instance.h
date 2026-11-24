#ifndef INSTANCE_H
#define INSTANCE_H

#include <stdio.h>
#include <stdlib.h>

/*
 * Tier 3 trunk: two switches run on one machine, so each needs its own
 * control socket and shared memory names. VSWITCH_NAME=a turns
 * "/vswitch_mac" into "/vswitch_mac_a". Without it the names are unchanged.
 */
static inline const char *inst_name(const char *base, char *buf, size_t n)
{
    const char *s = getenv("VSWITCH_NAME");
    if (s == NULL || s[0] == '\0') return base;
    snprintf(buf, n, "%s_%s", base, s);
    return buf;
}

#endif