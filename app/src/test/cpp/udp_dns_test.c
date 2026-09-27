/* udp_dns 純函式單元測試：鎖住非同步網域解析的簿記語意。
 *
 * 這一層的 bug 會直接表現成「QUIC 卡住」：job 槽挑錯 → 覆蓋別人正在解的工作；
 * pend 槽挑錯 → 補送時送到錯的 host；hold_ok 邊界錯 → 首包被丟或緩衝溢位。
 */
#include "udp_dns.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        g_failures++; \
    } \
} while (0)

static void set_job(udp_dns_job_t *j, int state, const char *host) {
    atomic_store(&j->state, state);
    snprintf(j->host, sizeof j->host, "%s", host);
}

/* state 是 atomic_int，不能用 memset 歸零（對 _Atomic 物件 memset 是未定義行為）。
 * 這裡逐格顯式初始化；host 一併清成空字串，避免殘留值影響 strcmp 判斷。 */
static void init_jobs(udp_dns_job_t *jobs, int n) {
    for (int i = 0; i < n; i++) {
        atomic_store(&jobs[i].state, UDP_DNS_FREE);
        jobs[i].host[0] = '\0';
        jobs[i].ip[0] = '\0';
    }
}

static void test_job_find(void) {
    udp_dns_job_t jobs[4];
    init_jobs(jobs, 4);

    /* 全空：找不到任何 host */
    CHECK(udp_dns_job_find(jobs, 4, "a.com") == -1);

    set_job(&jobs[2], UDP_DNS_INFLIGHT, "a.com");
    CHECK(udp_dns_job_find(jobs, 4, "a.com") == 2);

    /* FREE 槽的殘留字串不可以被當成命中（否則會蓋掉別人的解析結果） */
    atomic_store(&jobs[2].state, UDP_DNS_FREE);
    CHECK(udp_dns_job_find(jobs, 4, "a.com") == -1);

    /* 三種「佔用」狀態都算命中：INFLIGHT / DONE_OK / DONE_FAIL */
    set_job(&jobs[0], UDP_DNS_INFLIGHT, "b.com");
    set_job(&jobs[1], UDP_DNS_DONE_OK, "c.com");
    set_job(&jobs[3], UDP_DNS_DONE_FAIL, "d.com");
    CHECK(udp_dns_job_find(jobs, 4, "b.com") == 0);
    CHECK(udp_dns_job_find(jobs, 4, "c.com") == 1);
    CHECK(udp_dns_job_find(jobs, 4, "d.com") == 3);

    /* 未登錄的網域、空字串、NULL 一律 -1 */
    CHECK(udp_dns_job_find(jobs, 4, "e.com") == -1);
    CHECK(udp_dns_job_find(jobs, 4, "") == -1);
    CHECK(udp_dns_job_find(jobs, 4, NULL) == -1);

    /* 大小寫不同視為不同 host（這裡不做正規化，與 Java 端快取 key 的處理無關） */
    CHECK(udp_dns_job_find(jobs, 4, "B.com") == -1);

    /* n <= 0 或 NULL 表 */
    CHECK(udp_dns_job_find(jobs, 0, "b.com") == -1);
    CHECK(udp_dns_job_find(NULL, 4, "b.com") == -1);
}

static void test_job_alloc(void) {
    udp_dns_job_t jobs[3];
    init_jobs(jobs, 3);

    CHECK(udp_dns_job_alloc(jobs, 3) == 0);
    atomic_store(&jobs[0].state, UDP_DNS_INFLIGHT);
    CHECK(udp_dns_job_alloc(jobs, 3) == 1);
    atomic_store(&jobs[1].state, UDP_DNS_DONE_OK);
    CHECK(udp_dns_job_alloc(jobs, 3) == 2);
    atomic_store(&jobs[2].state, UDP_DNS_DONE_FAIL);

    /* 全滿：必須回 -1，讓呼叫端走「放棄解析」而不是覆蓋在用槽位 */
    CHECK(udp_dns_job_alloc(jobs, 3) == -1);

    /* 釋放中間一格後，alloc 只會挑那一格 */
    atomic_store(&jobs[1].state, UDP_DNS_FREE);
    CHECK(udp_dns_job_alloc(jobs, 3) == 1);

    CHECK(udp_dns_job_alloc(jobs, 0) == -1);
    CHECK(udp_dns_job_alloc(NULL, 3) == -1);
}

static void test_pend_slots(void) {
    udp_dns_pend_t pend[UDP_DNS_PEND_SLOTS];
    memset(pend, 0, sizeof pend);

    CHECK(udp_dns_pend_count(pend, UDP_DNS_PEND_SLOTS) == 0);
    CHECK(udp_dns_pend_free_slot(pend, UDP_DNS_PEND_SLOTS) == 0);

    pend[0].used = 1;
    snprintf(pend[0].host, sizeof pend[0].host, "a.com");
    CHECK(udp_dns_pend_count(pend, UDP_DNS_PEND_SLOTS) == 1);
    CHECK(udp_dns_pend_free_slot(pend, UDP_DNS_PEND_SLOTS) == 1);

    /* 佔滿：count 等於上限、free_slot 回 -1（呼叫端據此走「直接丟棄」） */
    for (int i = 0; i < UDP_DNS_PEND_SLOTS; i++) pend[i].used = 1;
    CHECK(udp_dns_pend_count(pend, UDP_DNS_PEND_SLOTS) == UDP_DNS_PEND_SLOTS);
    CHECK(udp_dns_pend_free_slot(pend, UDP_DNS_PEND_SLOTS) == -1);

    /* 歸零後 count 也歸零（閒置回收靠這個判斷「還有沒有欠資料」） */
    for (int i = 0; i < UDP_DNS_PEND_SLOTS; i++) pend[i].used = 0;
    CHECK(udp_dns_pend_count(pend, UDP_DNS_PEND_SLOTS) == 0);

    CHECK(udp_dns_pend_count(NULL, 4) == 0);
    CHECK(udp_dns_pend_free_slot(NULL, 4) == -1);
    CHECK(udp_dns_pend_count(pend, 0) == 0);
    CHECK(udp_dns_pend_free_slot(pend, 0) == -1);
}

static void test_hold_ok(void) {
    /* 空 payload 沒有意義，不值得暫存 */
    CHECK(udp_dns_hold_ok(0) == 0);
    CHECK(udp_dns_hold_ok(1) == 1);

    /* QUIC Initial 實務上 ~1200B，一定要留得住 */
    CHECK(udp_dns_hold_ok(1200) == 1);

    /* 邊界：等於上限可留，超過就不留（寧可丟掉讓上層重傳，也不能溢位） */
    CHECK(udp_dns_hold_ok(UDP_DNS_PEND_FRAME_MAX) == 1);
    CHECK(udp_dns_hold_ok(UDP_DNS_PEND_FRAME_MAX + 1) == 0);
    CHECK(udp_dns_hold_ok(65535) == 0);
}

int main(void) {
    test_job_find();
    test_job_alloc();
    test_pend_slots();
    test_hold_ok();
    if (g_failures == 0) {
        printf("udp_dns_test: ALL PASS\n");
        return 0;
    }
    printf("udp_dns_test: %d FAILED\n", g_failures);
    return 1;
}
