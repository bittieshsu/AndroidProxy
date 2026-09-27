#include "udp_dns.h"

#include <string.h>

int udp_dns_job_find(const udp_dns_job_t *jobs, int n, const char *host) {
    if (!jobs || n <= 0 || !host || !host[0]) return -1;
    for (int i = 0; i < n; i++) {
        if (jobs[i].state == UDP_DNS_FREE) continue;
        if (strcmp(jobs[i].host, host) == 0) return i;
    }
    return -1;
}

int udp_dns_job_alloc(const udp_dns_job_t *jobs, int n) {
    if (!jobs || n <= 0) return -1;
    for (int i = 0; i < n; i++) {
        if (jobs[i].state == UDP_DNS_FREE) return i;
    }
    return -1;
}

int udp_dns_pend_count(const udp_dns_pend_t *pend, int n) {
    if (!pend || n <= 0) return 0;
    int c = 0;
    for (int i = 0; i < n; i++) {
        if (pend[i].used) c++;
    }
    return c;
}

int udp_dns_pend_free_slot(const udp_dns_pend_t *pend, int n) {
    if (!pend || n <= 0) return -1;
    for (int i = 0; i < n; i++) {
        if (!pend[i].used) return i;
    }
    return -1;
}

int udp_dns_hold_ok(size_t payload_len) {
    if (payload_len == 0) return 0;
    return (payload_len <= UDP_DNS_PEND_FRAME_MAX) ? 1 : 0;
}
