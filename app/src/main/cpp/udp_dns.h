#ifndef UDP_DNS_H
#define UDP_DNS_H

#include <stddef.h>
#include <stdatomic.h>

/*
 * UDP relay 的「非同步網域解析」簿記（純邏輯：零 socket / epoll / JNI / 執行緒依賴）。
 *
 * 為什麼需要它：ATYP=0x03 的 frame 必須先解析成 IP 才能 sendto。舊版在 UDP worker
 * 的事件迴圈裡同步呼叫 Java DnsResolver（逾時上限 2 秒），一次逾時就凍結該 worker
 * 上「所有」session —— QUIC 的 ACK 與重傳全部停擺（worker 只有 4 條）。
 * 而且解析失敗時整個 frame 被靜默丟棄（不計數、不記 log），客戶端送出的 datagram
 * 永遠等不到回覆，上層只能一直重傳。
 *
 * 新設計把「解析」與「轉發」拆開：
 *   1. worker 查快取；未命中就佔一個 job 槽（state = INFLIGHT）丟進解析佇列，
 *      並把 frame 暫存進 session 的 pending 槽，然後立刻回去處理其他事件 —— 絕不阻塞。
 *   2. 背景執行緒解析完成後寫入 job 並喚醒 worker。
 *   3. worker 被喚醒後才把 pending frame 補送出去；失敗則記數並出聲。
 *
 * 本模組只負責「哪一格、要不要暫存、暫存到哪」的判斷；所有 I/O 留在呼叫端。
 */

#define UDP_DNS_HOST_MAX 256        /* 網域字串上限（含結尾 NUL） */
#define UDP_DNS_JOB_COUNT 32        /* 每個 UDP worker 同時在解的網域數上限 */
#define UDP_DNS_PEND_SLOTS 2        /* 每個 session 最多暫存幾個 frame */
#define UDP_DNS_PEND_FRAME_MAX 1280 /* 單一暫存 frame 的位元組上限（涵蓋 QUIC Initial ~1200B） */

typedef enum {
    UDP_DNS_FREE = 0,     /* 空槽 */
    UDP_DNS_INFLIGHT = 1, /* 已排入解析佇列，等背景執行緒 */
    UDP_DNS_DONE_OK = 2,  /* 解析成功，ip 已寫入，待 worker 取用 */
    UDP_DNS_DONE_FAIL = 3 /* 解析失敗，待 worker 記數後釋放 */
} udp_dns_state_t;

typedef struct {
    /* 狀態由解析執行緒寫入、UDP worker 讀取，兩邊以 release/acquire 配對，
     * 因此必須是 _Atomic 型別（C11 標準；Clang 會硬性要求，GCC 只是剛好放行
     * 非 _Atomic 的 int，所以「在 GCC 上編得過」不代表正確）。
     * 用 atomic_int 而非 int：NDK 26 的 clang 對 &j->state 會直接報
     * 「address argument to atomic operation must be a pointer to _Atomic type」。 */
    atomic_int state;
    char host[UDP_DNS_HOST_MAX];
    unsigned char port[2];
    /* 解析結果的 IP 字面值（state == DONE_OK 時才有意義）。
     * 大小刻意與 host 一致（而非剛好 46）：解析端寫入的來源緩衝就是這個尺寸，
     * 兩者相同時 GCC 才能證明 snprintf 不會截斷，不必為了 -Wformat-truncation
     * 在呼叫端加它推導不出的長度檢查。實際內容永遠只有 IP 字面值。 */
    char ip[UDP_DNS_HOST_MAX];
} udp_dns_job_t;

typedef struct {
    int used;
    char host[UDP_DNS_HOST_MAX];
    unsigned char port[2];
    size_t len;
    unsigned char payload[UDP_DNS_PEND_FRAME_MAX];
} udp_dns_pend_t;

/* 在 job 表找 host（只找非 FREE 的槽）。host 為 NULL/空字串，或找不到，回 -1。 */
int udp_dns_job_find(const udp_dns_job_t *jobs, int n, const char *host);

/* 找一個 FREE 槽的索引；沒有空槽回 -1。 */
int udp_dns_job_alloc(const udp_dns_job_t *jobs, int n);

/* 統計 pending 陣列中已佔用的槽數。 */
int udp_dns_pend_count(const udp_dns_pend_t *pend, int n);

/* 找 pending 陣列中第一個空槽；全滿回 -1。 */
int udp_dns_pend_free_slot(const udp_dns_pend_t *pend, int n);

/* 這個 payload 值不值得暫存等解析（超過 UDP_DNS_PEND_FRAME_MAX 就不值得，
 * 因為首包通常是 QUIC Initial ~1200B；太大的寧可丟掉讓上層重傳）。 */
int udp_dns_hold_ok(size_t payload_len);

#endif /* UDP_DNS_H */
