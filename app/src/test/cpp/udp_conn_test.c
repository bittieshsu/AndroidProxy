/* udp_conn 純函式單元測試：鎖住 UDP epoll worker 的世代驗證、fd 角色解碼與逾時語意。 */
#include "udp_conn.h"

#include <stdio.h>

static int g_failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        g_failures++; \
    } \
} while (0)

static void test_bad_slot(void) {
    CHECK(udp_event_bad_slot(0, 1088) == 0);
    CHECK(udp_event_bad_slot(1087, 1088) == 0);
    CHECK(udp_event_bad_slot(-1, 1088) == 1);
    CHECK(udp_event_bad_slot(1088, 1088) == 1);
    CHECK(udp_event_bad_slot(5000, 1088) == 1);
}

static void test_decode(void) {
    uint32_t sidx, egen;
    udp_fd_role_t role;

    /* Client TCP 角色 (無旗標) */
    udp_event_decode(((uint64_t)7u << 32) | 123u, &sidx, &egen, &role);
    CHECK(sidx == 123 && egen == 7 && role == UDP_ROLE_CLIENT);

    /* Local UDP 角色 (帶 LOCAL 旗標) */
    udp_event_decode(((uint64_t)7u << 32) | 123u | UDP_EV_ROLE_LOCAL_FLAG, &sidx, &egen, &role);
    CHECK(sidx == 123 && egen == 7 && role == UDP_ROLE_LOCAL);

    /* Remote 5G UDP 角色 (帶 REMOTE 旗標) */
    udp_event_decode(((uint64_t)7u << 32) | 123u | UDP_EV_ROLE_REMOTE_FLAG, &sidx, &egen, &role);
    CHECK(sidx == 123 && egen == 7 && role == UDP_ROLE_REMOTE);

    /* 邊界數值：大 slot、大 gen */
    udp_event_decode(((uint64_t)0x12345678u << 32) | 1087u | UDP_EV_ROLE_REMOTE_FLAG, &sidx, &egen, &role);
    CHECK(sidx == 1087 && egen == 0x12345678u && role == UDP_ROLE_REMOTE);
}

static void test_check_slot(void) {
    /* 正常有效事件 -> PROCESS */
    udp_event_disp_t d = udp_event_check_slot(10, 10, UDP_CONN_MAGIC, 1, 0, 1);
    CHECK(d == UDP_EV_PROCESS);

    /* 槽位未啟用 -> INACTIVE */
    d = udp_event_check_slot(10, 10, UDP_CONN_MAGIC, 0, 0, 1);
    CHECK(d == UDP_EV_INACTIVE);

    /* 世代不符 -> STALE */
    d = udp_event_check_slot(9, 10, UDP_CONN_MAGIC, 1, 0, 1);
    CHECK(d == UDP_EV_STALE);

    /* Magic 毒化 -> STALE */
    d = udp_event_check_slot(10, 10, 0, 1, 0, 1);
    CHECK(d == UDP_EV_STALE);

    /* Closed -> NOT_READY */
    d = udp_event_check_slot(10, 10, UDP_CONN_MAGIC, 1, 1, 1);
    CHECK(d == UDP_EV_NOT_READY);

    /* 未完成註冊 -> NOT_READY */
    d = udp_event_check_slot(10, 10, UDP_CONN_MAGIC, 1, 0, 0);
    CHECK(d == UDP_EV_NOT_READY);
}

static void test_idle_expired(void) {
    CHECK(udp_conn_idle_expired(100, 0, 60) == 0);
    CHECK(udp_conn_idle_expired(100, 50, 60) == 0); /* 50s idle <= 60s */
    CHECK(udp_conn_idle_expired(100, 40, 60) == 0); /* 60s idle <= 60s */
    CHECK(udp_conn_idle_expired(100, 39, 60) == 1); /* 61s idle > 60s */
}

/* [非同步解析] 喚醒識別碼必須與 shutdown 識別碼（0）及所有真實 session 事件互斥。
 * worker 的事件迴圈順序是：先比對 0（shutdown）、再比對喚醒值、最後才解碼；
 * 一旦碰撞，worker 會把某條 session 的事件當成喚醒，或反之 —— 兩者都很難查。 */
static void test_dns_wake(void) {
    CHECK(udp_event_is_dns_wake(UDP_EV_DNS_WAKE) == 1);

    /* shutdown pipe 用的全零識別碼不可以被當成喚醒 */
    CHECK(udp_event_is_dns_wake(0) == 0);

    /* 真實 session 事件：gen 由 1 起算且只遞增，slot 落在 [0, 1088)。
     * 掃過邊界組合，確認都不等於喚醒值。 */
    uint32_t gens[] = {1u, 2u, 0x7FFFFFFFu, 0xFFFFFFFEu};
    uint32_t slots[] = {0u, 1u, 1087u, 0xFFFFFFFFu};
    for (size_t gi = 0; gi < sizeof gens / sizeof gens[0]; gi++) {
        for (size_t si = 0; si < sizeof slots / sizeof slots[0]; si++) {
            uint64_t raw = ((uint64_t)gens[gi] << 32) | slots[si];
            CHECK(udp_event_is_dns_wake(raw) == 0);
            /* 帶角色旗標的變體同樣不得碰撞 */
            CHECK(udp_event_is_dns_wake(raw | UDP_EV_ROLE_LOCAL_FLAG) == 0);
            CHECK(udp_event_is_dns_wake(raw | UDP_EV_ROLE_REMOTE_FLAG) == 0);
        }
    }

    /* 縱深防禦：即使有人漏了喚醒比對，這個識別碼解出來的 slot 也必然越界，
     * 會被 udp_event_bad_slot 攔下（而不是誤打某一條真 session）。 */
    uint32_t sidx, egen;
    udp_fd_role_t role;
    udp_event_decode(UDP_EV_DNS_WAKE, &sidx, &egen, &role);
    CHECK(udp_event_bad_slot((int)sidx, 1088) == 1);
    CHECK(egen == 0xFFFFFFFFu);
}

int main(void) {
    test_bad_slot();
    test_decode();
    test_check_slot();
    test_idle_expired();
    test_dns_wake();
    if (g_failures == 0) {
        printf("udp_conn_test: ALL PASS\n");
        return 0;
    }
    printf("udp_conn_test: %d FAILED\n", g_failures);
    return 1;
}
