// epoll_integration_test.c
//
// host 整合測試：在純 Linux C 工具鏈（無 Android/JNI/socket 綁定）下，
// 直接驅動 simple-socks5.c 的公開 API，走完「accept → SOCKS5 握手 → CONNECT →
// 資料回流 → 關閉」，並以高併發 churn 壓測槽位分配/歸還、handoff、finalize 路徑。
//
// 關鍵：透過 stub 掉 4 個 extern（jni_attach_thread / jni_detach_thread /
// request_java_5g_socket / release_java_socket），把「Java 綁 5G 網路並 connect」
// 替換成 host 上真實的 socket()+connect()，讓整顆 epoll 引擎以真實 socket 運行，
// 但不需要 JVM。這樣才能用自動化測試鎖住最脆弱的核心（殘留事件/雙重釋放/洩漏），
// 這正是過去純函式單元測試碰不到的部分。
//
// 依賴 Linux 專屬的 <sys/epoll.h> 與 /proc/self/fd，因此只在 Linux CI 上編譯執行；
// Windows/MinGW 開發機由 Gradle 依 OS 門檻跳過本測試。

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <dirent.h>
#include <stdatomic.h>

// ---------- 公開 API（定義於 simple-socks5.c） ----------
extern void socks5_server_set_auth(const char *user, const char *pass);
extern void socks5_server_set_bind_addrs(const char **addrs, int count);
extern int  socks5_server_main_dynamic(int port);
extern int  socks5_server_is_running(void);
extern int  socks5_server_get_stats(char *out, size_t out_len);
extern int  socks5_server_get_bytes(long long *tx, long long *rx);
extern int  socks5_server_get_dns_stats(char *out, size_t out_len);
extern void socks5_server_quit(void);

// ---------- 測試參數 ----------
#define PROXY_PORT 21080
#define ECHO_PORT  21081
#define UDP_ECHO_PORT 21082
#define CHURN_THREADS 8
#define CHURN_ITER     30

static volatile int g_echo_stop = 0;
static int g_last_connect_errno = 0;

// ---------- JNI stub：simple-socks5.c 內宣告的 extern ----------
void jni_attach_thread(void) {}
void jni_detach_thread(void) {}

// release_java_socket 在 host 測試為 no-op：真實裝置上它負責關閉 Java 端持有的
// dup 副本；host 上沒有 Java Socket，C 端 close() 已足夠關閉描述。
void release_java_socket(int fd) { (void)fd; }

// request_java_5g_socket 的 host 替代：以 getaddrinfo 解析 host、真實 connect，
// 回傳已連線的 fd（blocking，與真實 Java 端在握手執行緒上做 blocking connect 一致）。
static int host_connect(const char *host, int port) {
    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", port);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, portstr, &hints, &res) != 0) return -1;
    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

int request_java_5g_socket(const char *host, int port, int is_udp) {
    if (is_udp) {
        // 雙棧 UDP：與真實 Java 端一樣優先 IPv6，失敗退 IPv4
        int fd = socket(AF_INET6, SOCK_DGRAM, 0);
        if (fd >= 0) {
            int v6only = 0;
            setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
            return fd;
        }
        fd = socket(AF_INET, SOCK_DGRAM, 0);
        return fd;
    }
    return host_connect(host, port);
}

// ---------- request_java_resolve_host 的 host 替代 ----------
// 真實裝置上這會走 Java 的 DnsResolver（蜂巢式網路 + 共用快取 + 逾時）。
// host 測試用「可控延遲的假解析」取代，才能驗證 UDP worker 在解析期間沒有被阻塞。
static atomic_int g_resolve_delay_ms = 0;   // 每次解析的人為延遲
static atomic_int g_resolve_calls = 0;      // 解析被呼叫的次數（驗證去重）
static char g_resolve_fail_host[128] = {0}; // 指定這個 host 一律解析失敗

int request_java_resolve_host(const char *host, char *out, size_t out_len) {
    atomic_fetch_add(&g_resolve_calls, 1);
    if (!out || out_len == 0) return -1;
    out[0] = '\0';
    int d = atomic_load(&g_resolve_delay_ms);
    if (d > 0) usleep((useconds_t)d * 1000);
    if (g_resolve_fail_host[0] && strcmp(host, g_resolve_fail_host) == 0) return -1;
    // 測試用的假 DNS：任何網域都指向 loopback，於是「5G 出口」就是本機的 UDP echo。
    snprintf(out, out_len, "%s", "127.0.0.1");
    return 0;
}

// ---------- echo server（CONNECT 的目標） ----------
static void *echo_conn(void *arg) {
    int fd = (int)(long)arg;
    char buf[4096];
    for (;;) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        ssize_t off = 0;
        while (off < n) {
            ssize_t s = send(fd, buf + off, n - off, 0);
            if (s <= 0) { n = 0; break; }
            off += s;
        }
        if ((size_t)off != (size_t)n) break;
    }
    close(fd);
    return NULL;
}

static void *echo_server(void *arg) {
    (void)arg;
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("echo socket"); return NULL; }
    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons(ECHO_PORT);
    if (bind(srv, (struct sockaddr *)&sa, sizeof(sa)) < 0 ||
        listen(srv, 1024) < 0) {
        perror("echo bind/listen");
        close(srv);
        return NULL;
    }
    // [測試修復] 設非阻塞並在每次 poll 喚醒後把 accept 佇列一次排空：
    // 原先單次 accept + 每連線 pthread_create 在 16 執行緒 churn 下讓 backlog
    // 短暫堆滿，host_connect(echo) 被 ECONNREFUSED → 代理正確回 REP=0x04（非引擎
    // bug，但會讓 churn 統計出現假性失敗）。排空後 backlog 不再堆積。
    int fl = fcntl(srv, F_GETFL, 0);
    fcntl(srv, F_SETFL, fl | O_NONBLOCK);
    while (!g_echo_stop) {
        struct pollfd pfd = { srv, POLLIN, 0 };
        if (poll(&pfd, 1, 100) <= 0) continue;
        for (;;) {
            int c = accept(srv, NULL, NULL);
            if (c < 0) break; // EAGAIN：已排空
            pthread_t t;
            if (pthread_create(&t, NULL, echo_conn, (void *)(long)c) == 0) {
                pthread_detach(t);
            } else {
                close(c);
            }
        }
    }
    close(srv);
    return NULL;
}

// ---------- 測試工具：blocking 收發（帶 timeout 防卡死） ----------
static void set_io_timeout(int fd, int sec) {
    struct timeval tv = { sec, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv, sizeof(tv));
}

static int connect_proxy_retry(void) {
    for (int i = 0; i < 50; i++) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return -1;
        struct sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        sa.sin_port = htons(PROXY_PORT);
        if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0) {
            set_io_timeout(fd, 5);
            return fd;
        }
        g_last_connect_errno = errno;
        close(fd);
        usleep(50 * 1000); // 50ms
    }
    return -1;
}

// 完整走一次：握手(無認證) → CONNECT(127.0.0.1:ECHO_PORT) → 回送 payload 驗證 echo。
// 成功回傳 0，任何一步失敗回傳 -1。
static int do_roundtrip(const char *payload, int payload_len) {
    int c = connect_proxy_retry();
    if (c < 0) return -1;

    unsigned char buf[512];

    // 1. 握手：VER=5, NMETHODS=1, METHOD=0x00 (no auth)
    buf[0] = 0x05; buf[1] = 0x01; buf[2] = 0x00;
    if (send(c, buf, 3, 0) != 3) goto fail;
    if (recv(c, buf, 2, MSG_WAITALL) != 2) goto fail;
    if (buf[0] != 0x05 || buf[1] != 0x00) goto fail;

    // 2. CONNECT：VER=5 CMD=1 RSV=0 ATYP=0x01 127.0.0.1:ECHO_PORT
    buf[0] = 0x05; buf[1] = 0x01; buf[2] = 0x00; buf[3] = 0x01;
    buf[4] = 127; buf[5] = 0; buf[6] = 0; buf[7] = 1;
    buf[8] = (unsigned char)(ECHO_PORT >> 8);
    buf[9] = (unsigned char)(ECHO_PORT & 0xFF);
    if (send(c, buf, 10, 0) != 10) goto fail;
    // 成功回覆 10 bytes：VER REP RSV ATYP BND.ADDR(4) BND.PORT(2)
    if (recv(c, buf, 10, MSG_WAITALL) != 10) goto fail;
    if (buf[0] != 0x05 || buf[1] != 0x00) goto fail;

    // 3. 資料回流：送 payload，期待 echo 完整回傳
    if (send(c, payload, payload_len, 0) != payload_len) goto fail;
    int got = 0;
    while (got < payload_len) {
        int n = recv(c, buf, sizeof(buf), 0);
        if (n <= 0) goto fail;
        if (got + n > payload_len) goto fail;
        if (memcmp(buf, payload + got, n) != 0) goto fail;
        got += n;
    }

    close(c);
    return 0;

fail:
    close(c);
    return -1;
}

// ---------- churn 執行緒：快速建立/拆除連線，壓測槽位與 finalize ----------
typedef struct { int id; int failures; int f_connect; int f_handshake; int f_connect_reply; } churn_arg_t;

static void *churn_worker(void *arg) {
    churn_arg_t *ca = (churn_arg_t *)arg;
    unsigned char buf[32];
    for (int i = 0; i < CHURN_ITER; i++) {
        // 平滑 burst：代理 listen backlog(128) 加上握手執行緒內的 blocking echo
        // connect，在 16 執行緒無間隔 churn 下會被瞬間灌爆（ECONNREFUSED）。
        // 以 1ms 間隔讓 accept→握手管線跟上，churn 仍以 8 執行緒並行壓測 slot/finalize。
        usleep(1000);
        int c = connect_proxy_retry();
        if (c < 0) { ca->failures++; ca->f_connect++; continue; }
        // 握手
        buf[0] = 0x05; buf[1] = 0x01; buf[2] = 0x00;
        if (send(c, buf, 3, 0) != 3) { ca->failures++; ca->f_handshake++; close(c); continue; }
        if (recv(c, buf, 2, MSG_WAITALL) != 2 || buf[1] != 0x00) { ca->failures++; ca->f_handshake++; close(c); continue; }
        // CONNECT
        buf[0] = 0x05; buf[1] = 0x01; buf[2] = 0x00; buf[3] = 0x01;
        buf[4] = 127; buf[5] = 0; buf[6] = 0; buf[7] = 1;
        buf[8] = (unsigned char)(ECHO_PORT >> 8);
        buf[9] = (unsigned char)(ECHO_PORT & 0xFF);
        if (send(c, buf, 10, 0) != 10) { ca->failures++; ca->f_connect_reply++; close(c); continue; }
        // CONNECT 回覆：REP==0x00 成功；REP==0x04 代表 host_connect(echo) 被拒
        if (recv(c, buf, 10, MSG_WAITALL) != 10 || buf[1] != 0x00) {
            ca->failures++; ca->f_connect_reply++;
            close(c); continue;
        }
        // 立即關閉（teardown 路徑）
        close(c);
    }
    return NULL;
}

// 從 "conns=N acquired=..." 統計字串中提取欄位；找不到回傳 -1。
static long long stat_field(const char *s, const char *key) {
    char pat[32];
    snprintf(pat, sizeof(pat), "%s=%%lld", key);
    long long v = -1;
    // 逐 token 掃描，避免 sscanf 對順序的依賴
    const char *p = strstr(s, key);
    if (p) sscanf(p, pat, &v);
    return v;
}

// ================= UDP-in-TCP（cmd=0x04）整合測試素材 =================
//
// 目的：鎖住「UDP worker 不得被網域解析阻塞」這個回歸。
// 舊版在 worker 的事件迴圈裡同步呼叫 Java DnsResolver（逾時上限 2 秒），一次逾時
// 就凍結該 worker 上所有 session —— 這正是 Cloudflare Turnstile 卡住的根因。
// 新版的判別性探針是：同一條 0x04 session 上「先送一個網域 frame（會觸發解析延遲）、
// 緊接著送一個 IP 字面值 frame」，兩者一次寫入。若 worker 仍會阻塞，第二個 frame
// 的回覆必然被第一個的解析延遲拖住；非同步則幾乎立刻回覆。

static void *udp_echo_server(void *arg) {
    (void)arg;
    int srv = socket(AF_INET, SOCK_DGRAM, 0);
    if (srv < 0) { perror("udp echo socket"); return NULL; }
    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons(UDP_ECHO_PORT);
    if (bind(srv, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        perror("udp echo bind");
        close(srv);
        return NULL;
    }
    while (!g_echo_stop) {
        struct pollfd pfd = { srv, POLLIN, 0 };
        if (poll(&pfd, 1, 100) <= 0) continue;
        char buf[2048];
        struct sockaddr_storage from;
        socklen_t fl = sizeof(from);
        ssize_t n = recvfrom(srv, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
        if (n <= 0) continue;
        sendto(srv, buf, (size_t)n, 0, (struct sockaddr *)&from, fl);
    }
    close(srv);
    return NULL;
}

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// 開一條 UDP-in-TCP(0x04) session：握手(無認證) → cmd=0x04 → 期待 REP=0x00。
static int udp_in_tcp_open(void) {
    int c = connect_proxy_retry();
    if (c < 0) return -1;
    unsigned char buf[16];
    buf[0] = 0x05; buf[1] = 0x01; buf[2] = 0x00;
    if (send(c, buf, 3, 0) != 3) goto fail;
    if (recv(c, buf, 2, MSG_WAITALL) != 2) goto fail;
    if (buf[0] != 0x05 || buf[1] != 0x00) goto fail;
    buf[0] = 0x05; buf[1] = 0x04; buf[2] = 0x00; buf[3] = 0x01;
    buf[4] = 0; buf[5] = 0; buf[6] = 0; buf[7] = 0; buf[8] = 0; buf[9] = 0;
    if (send(c, buf, 10, 0) != 10) goto fail;
    if (recv(c, buf, 10, MSG_WAITALL) != 10) goto fail;
    if (buf[0] != 0x05 || buf[1] != 0x00) goto fail;
    return c;
fail:
    close(c);
    return -1;
}

// 組一個 UDP-in-TCP frame：2-byte 長度欄 + SOCKS5 UDP datagram。
// addr 對 0x01/0x04 是 IP 位元組；對 0x03 是「長度 + 網域」。
static int udp_build_frame(unsigned char *out, size_t cap, int atyp,
                           const unsigned char *addr, int addr_len,
                           int port, const char *payload, int plen) {
    int dlen = 3 + 1 + addr_len + 2 + plen; // RSV(2)+FRAG(1)+ATYP(1)+ADDR+PORT(2)+DATA
    if (dlen < 0 || (size_t)dlen + 2 > cap) return -1;
    out[0] = (unsigned char)(dlen >> 8);
    out[1] = (unsigned char)(dlen & 0xFF);
    unsigned char *p = out + 2;
    p[0] = 0x00; p[1] = 0x00; p[2] = 0x00; p[3] = (unsigned char)atyp;
    memcpy(p + 4, addr, (size_t)addr_len);
    p[4 + addr_len]     = (unsigned char)(port >> 8);
    p[4 + addr_len + 1] = (unsigned char)(port & 0xFF);
    memcpy(p + 4 + addr_len + 2, payload, (size_t)plen);
    return dlen + 2;
}

// 收一個 UDP-in-TCP frame（先 2-byte 長度、再 body）。成功回 body 長度，逾時/錯誤回 -1。
static int udp_recv_frame(int fd, unsigned char *body, size_t cap, int timeout_ms) {
    struct pollfd pfd = { fd, POLLIN, 0 };
    if (poll(&pfd, 1, timeout_ms) <= 0) return -1;
    unsigned char lb[2];
    if (recv(fd, lb, 2, MSG_WAITALL) != 2) return -1;
    int dlen = (lb[0] << 8) | lb[1];
    if (dlen <= 0 || (size_t)dlen > cap) return -1;
    if (recv(fd, body, dlen, MSG_WAITALL) != dlen) return -1;
    return dlen;
}

// 從 SOCKS5 UDP datagram 取出 payload 起點與長度；成功回長度，格式錯誤回 -1。
static int udp_frame_payload(const unsigned char *body, int dlen, const unsigned char **payload) {
    if (dlen < 4) return -1;
    int atyp = body[3], hl;
    if (atyp == 0x01)      hl = 4 + 4 + 2;
    else if (atyp == 0x04) hl = 4 + 16 + 2;
    else if (atyp == 0x03) hl = 4 + 1 + body[4] + 2;
    else return -1;
    if (hl > dlen) return -1;
    *payload = body + hl;
    return dlen - hl;
}

// ================= [listener 重建] 測試素材 =================
//
// 目的：鎖住「介面消失後 listener 必須自行重建」這個回歸。
//
// 實機實測（小米 Pad Mini / Android 15，2026-09-27）：Wi-Fi 關閉時核心會直接把綁在
// wlan0 位址上的 listener 關掉（/proc/net/tcp 只剩 loopback），Wi-Fi 開回來後即使
// IPv4 完全沒變也依然只剩 loopback —— 舊版 listener 只在啟動時綁一次、之後永不重建，
// 於是代理顯示「運行中」卻永遠連不上，只能手動重啟服務。
//
// host 上可完全重現的兩個探針：
//   A. 執行期更新 desired 集合 → 新位址必須在數秒內被綁上、被移除的位址必須下線，
//      且既有連線必須存活（不得為了換 listener 而重建代理）。
//   B. 直接把引擎的 listener fd 關掉（模擬核心所為）→ 引擎必須自行重建它。
//      少了 B，A 有可能靠「啟動時綁一次」假通過；兩者一起才鎖得住這個機制。

// 連到 ip:port；成功回 fd（含 I/O 逾時），失敗回 -1。
static int connect_to(const char *ip, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &sa.sin_addr) != 1) { close(fd); return -1; }
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) { close(fd); return -1; }
    set_io_timeout(fd, 5);
    return fd;
}

// 重試版：等待 listener 被建立（引擎對帳節奏約 1 秒，這裡留足餘裕）。
static int connect_to_retry(const char *ip, int port, int attempts, int interval_ms) {
    for (int i = 0; i < attempts; i++) {
        int fd = connect_to(ip, port);
        if (fd >= 0) return fd;
        usleep((useconds_t)interval_ms * 1000);
    }
    return -1;
}

// 在已連上代理的 fd 上完成握手 + CONNECT(target_ip:target_port)，成功回 0。
static int proxy_open_tunnel(int c, const char *target_ip, int target_port) {
    unsigned char buf[16];
    buf[0] = 0x05; buf[1] = 0x01; buf[2] = 0x00;
    if (send(c, buf, 3, 0) != 3) return -1;
    if (recv(c, buf, 2, MSG_WAITALL) != 2) return -1;
    if (buf[0] != 0x05 || buf[1] != 0x00) return -1;

    struct in_addr a;
    if (inet_pton(AF_INET, target_ip, &a) != 1) return -1;
    buf[0] = 0x05; buf[1] = 0x01; buf[2] = 0x00; buf[3] = 0x01;
    memcpy(buf + 4, &a, 4);
    buf[8] = (unsigned char)(target_port >> 8);
    buf[9] = (unsigned char)(target_port & 0xFF);
    if (send(c, buf, 10, 0) != 10) return -1;
    if (recv(c, buf, 10, MSG_WAITALL) != 10) return -1;
    if (buf[0] != 0x05 || buf[1] != 0x00) return -1;
    return 0;
}

// 在既有隧道上送 payload 並驗證 echo 完整回傳；成功回 0。
static int echo_on_fd(int c, const char *payload, int payload_len) {
    if (send(c, payload, payload_len, 0) != payload_len) return -1;
    unsigned char buf[256];
    int got = 0;
    while (got < payload_len) {
        int n = recv(c, buf, sizeof(buf), 0);
        if (n <= 0) return -1;
        if (got + n > payload_len) return -1;
        if (memcmp(buf, payload + got, n) != 0) return -1;
        got += n;
    }
    return 0;
}

// 找出引擎綁在 ip:port 上的 listener fd（以 SO_ACCEPTCONN + getsockname 認身分）。
// 找不到回 -1。用於模擬「核心在介面消失時把 listener 關掉」。
static int find_listener_fd(const char *ip, int port) {
    DIR *d = opendir("/proc/self/fd");
    if (!d) return -1;
    int found = -1;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        int fd = atoi(e->d_name);
        int acceptconn = 0;
        socklen_t ol = sizeof(acceptconn);
        if (getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &acceptconn, &ol) != 0) continue;
        if (!acceptconn) continue;
        struct sockaddr_in sa;
        socklen_t sl = sizeof(sa);
        if (getsockname(fd, (struct sockaddr *)&sa, &sl) != 0) continue;
        if (sa.sin_family != AF_INET) continue;
        if (ntohs(sa.sin_port) != (uint16_t)port) continue;
        char buf[INET_ADDRSTRLEN] = {0};
        if (!inet_ntop(AF_INET, &sa.sin_addr, buf, sizeof(buf))) continue;
        if (strcmp(buf, ip) == 0) { found = fd; break; }
    }
    closedir(d);
    return found;
}

// 讀 /proc/self/fd/<fd> 的連結取得 socket inode；不是 socket 或讀不到回 0。
//
// 為什麼非用它不可：fd 編號會被重用 —— 關掉一個 listener 後，引擎重建的新 listener
// 極可能拿到同一個編號（實測兩次都是 29）。所以「fd 編號不同」不能當作重建的證據，
// 「inode 不同」才能。同理，bare connect() 也不能當判據：關閉當下引擎可能正卡在
// poll() 內，那個 syscall 對 fd 的引用會讓舊 socket 多活到 poll 返回，期間連線仍會
// 被排進它的 backlog（連得上但沒人服務）。因此重建後必須走完整 SOCKS5 握手。
static unsigned long long fd_socket_inode(int fd) {
    char path[64];
    char buf[128] = {0};
    snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
    if (readlink(path, buf, sizeof(buf) - 1) <= 0) return 0;
    if (strncmp(buf, "socket:[", 8) != 0) return 0;
    return strtoull(buf + 8, NULL, 10);
}

int main(void) {
    int failed = 0;

    // 1. 起 echo server（TCP 供 CONNECT 用；UDP 供 UDP-in-TCP 用）
    pthread_t echo_tid;
    pthread_create(&echo_tid, NULL, echo_server, NULL);
    pthread_t udp_echo_tid;
    pthread_create(&udp_echo_tid, NULL, udp_echo_server, NULL);

    // 2. 起代理（只綁 loopback）
    const char *addrs[] = { "127.0.0.1" };
    socks5_server_set_auth("", "");
    socks5_server_set_bind_addrs(addrs, 1);
    if (socks5_server_main_dynamic(PROXY_PORT) != 0) {
        fprintf(stderr, "FAIL: socks5_server_main_dynamic returned error\n");
        return 1;
    }

    // 3. 等待 listener 就緒（connect 重試內部已處理，這裡只等 running 旗標）
    for (int i = 0; i < 100 && !socks5_server_is_running(); i++) usleep(20 * 1000);
    if (!socks5_server_is_running()) {
        fprintf(stderr, "FAIL: server did not start\n");
        socks5_server_quit();
        return 1;
    }

    // 4. 單一連線完整回流
    const char *msg = "hello-5g-proxy";
    if (do_roundtrip(msg, (int)strlen(msg)) != 0) {
        fprintf(stderr, "FAIL: single round-trip echo\n");
        failed = 1;
    } else {
        printf("PASS: single round-trip echo\n");
    }

    // 4b. [流量統計回歸] 送出一段已知大小的資料，確認 tx（上傳）與 rx（下載）都
    //     真的被計到。舊寫法以 try_send 前後的 *off 差值推導上傳量，但完整排空時
    //     *off 會被歸零，差值變成 0 或負數 → 上傳幾乎完全不計（實測 290MB 只記到
    //     1.3MB），下載因在 recv 後直接累加而正常。此處以真實資料流同時鎖住兩者。
    {
        enum { ACCT_LEN = 64 * 1024 };
        static char big[ACCT_LEN];
        for (int i = 0; i < ACCT_LEN; i++) big[i] = (char)(i * 31 + 7);

        long long tx0 = 0, rx0 = 0;
        socks5_server_get_bytes(&tx0, &rx0);
        if (do_roundtrip(big, ACCT_LEN) != 0) {
            fprintf(stderr, "FAIL: 64KB round-trip echo\n");
            failed = 1;
        } else {
            long long tx = 0, rx = 0;
            socks5_server_get_bytes(&tx, &rx);
            long long dtx = tx - tx0, drx = rx - rx0;
            printf("traffic delta: tx=%lld rx=%lld (payload=%d)\n", dtx, drx, ACCT_LEN);
            if (dtx < ACCT_LEN) {
                fprintf(stderr, "FAIL: upload tx undercounted: %lld < %d\n", dtx, ACCT_LEN);
                failed = 1;
            } else {
                printf("PASS: upload tx counted (%lld bytes)\n", dtx);
            }
            if (drx < ACCT_LEN) {
                fprintf(stderr, "FAIL: download rx undercounted: %lld < %d\n", drx, ACCT_LEN);
                failed = 1;
            }
        }
    }

    // 4c. [非同步解析回歸] UDP-in-TCP 的網域 frame 不得阻塞 UDP worker。
    //
    // 判別性探針：同一條 0x04 session 上一次寫入兩個 frame ——
    //   frame A：ATYP=0x03 網域（觸發解析，人為延遲 RESOLVE_DELAY_MS）
    //   frame B：ATYP=0x01 IP 字面值（不需要解析）
    // 非同步版：B 的回覆幾乎立刻到（worker 只把 A 丟進佇列就繼續跑）。
    // 同步版（舊行為）：worker 卡在 A 的解析上，B 的回覆要等 RESOLVE_DELAY_MS 才出現。
    // 因此「B 的回覆是否早於延遲」就是區分新舊行為的唯一判據。
    {
        const int RESOLVE_DELAY_MS = 1200;
        atomic_store(&g_resolve_delay_ms, RESOLVE_DELAY_MS);
        atomic_store(&g_resolve_calls, 0);

        int c = udp_in_tcp_open();
        if (c < 0) {
            fprintf(stderr, "FAIL: 0x04 UDP-in-TCP handshake\n");
            failed = 1;
        } else {
            unsigned char out[512];
            const char *host = "slow.example";
            int hl = (int)strlen(host);
            unsigned char hostaddr[1 + 32];
            hostaddr[0] = (unsigned char)hl;
            memcpy(hostaddr + 1, host, (size_t)hl);
            unsigned char ip4[4] = { 127, 0, 0, 1 };

            int fa = udp_build_frame(out, sizeof out, 0x03, hostaddr, 1 + hl,
                                     UDP_ECHO_PORT, "A", 1);
            int fb = (fa > 0)
                   ? udp_build_frame(out + fa, sizeof out - (size_t)fa, 0x01, ip4, 4,
                                     UDP_ECHO_PORT, "B", 1)
                   : -1;
            if (fa < 0 || fb < 0 ||
                send(c, out, (size_t)(fa + fb), 0) != fa + fb) {
                fprintf(stderr, "FAIL: 送 UDP-in-TCP frame (fa=%d fb=%d)\n", fa, fb);
                failed = 1;
            } else {
                unsigned char body[256];
                long long t0 = now_ms();
                int d1 = udp_recv_frame(c, body, sizeof body, 5000);
                long long elapsed = now_ms() - t0;
                const unsigned char *pl = NULL;
                int pl1 = (d1 > 0) ? udp_frame_payload(body, d1, &pl) : -1;

                if (pl1 != 1 || pl[0] != 'B') {
                    // 若這裡拿到 'A'，代表 worker 先處理完網域 frame 才輪到 IP frame。
                    fprintf(stderr, "FAIL: 首個回覆不是 IP 字面值 frame (d=%d pl=%d%s)\n",
                            d1, pl1, (pl1 == 1 && pl) ? (pl[0] == 'A' ? " 收到A" : "") : "");
                    failed = 1;
                } else if (elapsed >= RESOLVE_DELAY_MS / 2) {
                    fprintf(stderr, "FAIL: IP 字面值 frame 被解析阻塞 %lldms (>= %dms)\n",
                            elapsed, RESOLVE_DELAY_MS / 2);
                    failed = 1;
                } else {
                    printf("PASS: 解析期間 UDP worker 未被阻塞（IP frame 回覆 %lldms < %dms）\n",
                           elapsed, RESOLVE_DELAY_MS / 2);
                }

                // 網域 frame 的補送回覆：解析完成後才該出現，內容必須正確
                int d2 = udp_recv_frame(c, body, sizeof body, 5000);
                const unsigned char *pl2 = NULL;
                int pl2len = (d2 > 0) ? udp_frame_payload(body, d2, &pl2) : -1;
                if (pl2len != 1 || pl2[0] != 'A') {
                    fprintf(stderr, "FAIL: 網域 frame 未被補送 (d=%d pl=%d)\n", d2, pl2len);
                    failed = 1;
                } else {
                    printf("PASS: 網域 frame 解析後補送成功\n");
                }
            }
            close(c);
        }

        // 診斷計數：解析有出聲、補送有發生、且沒有靜默丟棄。
        // 舊版「解析失敗 → 整個 frame 靜默丟棄」正是驗證頁卡死時完全沒有線索的原因。
        char ds[256] = { 0 };
        socks5_server_get_dns_stats(ds, sizeof ds);
        printf("dns stats: %s (resolve_calls=%d)\n", ds, atomic_load(&g_resolve_calls));
        long long enq     = stat_field(ds, "enq");
        long long replay  = stat_field(ds, "replay");
        long long drop    = stat_field(ds, "drop");
        long long qfull   = stat_field(ds, "qfull");
        long long jobfull = stat_field(ds, "jobfull");
        long long lost    = stat_field(ds, "lost");
        if (enq < 1)     { fprintf(stderr, "FAIL: dns enq=%lld\n", enq);         failed = 1; }
        if (replay < 1)  { fprintf(stderr, "FAIL: dns replay=%lld\n", replay);   failed = 1; }
        if (drop != 0)   { fprintf(stderr, "FAIL: dns drop=%lld\n", drop);       failed = 1; }
        if (qfull != 0)  { fprintf(stderr, "FAIL: dns qfull=%lld\n", qfull);     failed = 1; }
        if (jobfull != 0){ fprintf(stderr, "FAIL: dns jobfull=%lld\n", jobfull); failed = 1; }
        if (lost != 0)   { fprintf(stderr, "FAIL: dns lost=%lld\n", lost);       failed = 1; }
        if (failed == 0) printf("PASS: dns 計數乾淨（enq=%lld replay=%lld drop=0）\n", enq, replay);

        atomic_store(&g_resolve_delay_ms, 0);
    }

    // 5. 高併發 churn
    pthread_t th[CHURN_THREADS];
    churn_arg_t args[CHURN_THREADS];
    for (int i = 0; i < CHURN_THREADS; i++) {
        args[i].id = i;
        args[i].failures = 0;
        args[i].f_connect = 0;
        args[i].f_handshake = 0;
        args[i].f_connect_reply = 0;
        pthread_create(&th[i], NULL, churn_worker, &args[i]);
    }
    int total_fail = 0, tc = 0, thh = 0, tcr = 0;
    for (int i = 0; i < CHURN_THREADS; i++) {
        pthread_join(th[i], NULL);
        total_fail += args[i].failures;
        tc += args[i].f_connect;
        thh += args[i].f_handshake;
        tcr += args[i].f_connect_reply;
    }
    int total_attempts = CHURN_THREADS * CHURN_ITER;
    int success = total_attempts - total_fail;
    double rate = 100.0 * (double)success / (double)total_attempts;
    printf("churn done: %d threads x %d iters, success=%d/%d (%.1f%%) connect=%d handshake=%d connect_reply=%d last_connect_errno=%d\n",
           CHURN_THREADS, CHURN_ITER, success, total_attempts, rate,
           tc, thh, tcr, g_last_connect_errno);
    // 連線在 burst 下被拒絕是代理的設計行為（握手佇列滿即丟棄，屬背壓而非故障）。
    // churn 的真正目的是壓測槽位分配/歸還與 finalize（見下方安全不變式斷言），
    // 而非「零失敗」。以 >=90% 成功率當硬門檻，既不會把負載背壓誤判為引擎故障，
    // 又能捕捉「整段代理掛掉」這類嚴重回歸。
    if (rate < 90.0) {
        fprintf(stderr, "FAIL: churn success rate %.1f%% < 90%%\n", rate);
        failed = 1;
    }

    // 6. 等待 worker 把 churn 的連線全部 finalize（grace period 約 2 秒 + 餘裕）
    {
        char stats[512];
        for (int i = 0; i < 100; i++) {
            socks5_server_get_stats(stats, sizeof(stats));
            long long conns = stat_field(stats, "conns");
            if (conns == 0) break;
            usleep(100 * 1000);
        }
        socks5_server_get_stats(stats, sizeof(stats));
        printf("stats(before quit): %s\n", stats);

        long long acquired = stat_field(stats, "acquired");
        long long released = stat_field(stats, "released");
        long long stale    = stat_field(stats, "stale_skip");
        long long bad_slot = stat_field(stats, "bad_slot");
        long long purged   = stat_field(stats, "purged");

        // [關鍵斷言] 殘留事件（stale_skip）/槽位錯誤（bad_slot）/幽靈清除（purged）
        // 任一 >0 代表「世代替換防禦被實際觸發」，即潛在 UAF 防線被擊穿的前兆；
        // acquired != released 代表槽位洩漏（conn_finalize 未歸還）。
        if (stale > 0)   { fprintf(stderr, "FAIL: stale_skip=%lld\n", stale);   failed = 1; }
        if (bad_slot > 0){ fprintf(stderr, "FAIL: bad_slot=%lld\n",  bad_slot); failed = 1; }
        if (purged > 0)  { fprintf(stderr, "FAIL: purged=%lld\n",    purged);   failed = 1; }
        if (acquired != released) {
            fprintf(stderr, "FAIL: slot leak acquired=%lld released=%lld\n", acquired, released);
            failed = 1;
        }
        if (failed == 0) printf("PASS: no ghost events / no slot leak\n");
    }

    // 6b. [listener 重建] 執行期更新綁定位址 + 模擬核心關掉 listener 後自行重建。
    //     放在槽位/洩漏斷言之後，避免本段額外的連線影響上面那組不變式。
    {
        // (a) 既有連線：先經 loopback 建一條隧道並確認可用（等一下要用它驗證
        //     「換 listener 不得切斷既有連線」）。
        int keep = connect_to("127.0.0.1", PROXY_PORT);
        int have_keep = (keep >= 0 && proxy_open_tunnel(keep, "127.0.0.1", ECHO_PORT) == 0
                         && echo_on_fd(keep, "before-rebind", 13) == 0);
        if (!have_keep) {
            fprintf(stderr, "FAIL: rebind 前無法建立既有連線\n");
            failed = 1;
        } else {
            printf("PASS: rebind 前既有連線可用\n");
        }

        // (b) 執行期把 127.0.0.2 加入 desired 集合（127.0.0.0/8 在 Linux 上免設定即可綁）
        const char *addrs_new[] = { "127.0.0.2" };
        socks5_server_set_bind_addrs(addrs_new, 1);
        int c_new = connect_to_retry("127.0.0.2", PROXY_PORT, 40, 100);
        if (c_new < 0) {
            fprintf(stderr, "FAIL: 執行期新增的綁定位址沒有被綁上（127.0.0.2:%d）\n", PROXY_PORT);
            failed = 1;
        } else {
            printf("PASS: 執行期新增綁定位址已生效\n");
            close(c_new);
        }

        // (c) 既有連線必須存活 —— 換 listener 不可以是「重建代理」
        if (have_keep && echo_on_fd(keep, "after-rebind", 12) != 0) {
            fprintf(stderr, "FAIL: 更新綁定位址切斷了既有連線\n");
            failed = 1;
        } else if (have_keep) {
            printf("PASS: 更新綁定位址後既有連線存活\n");
        }

        // (d) 模擬核心在介面消失時關掉 listener → 引擎必須自行重建
        {
            int lfd = find_listener_fd("127.0.0.2", PROXY_PORT);
            if (lfd < 0) {
                fprintf(stderr, "FAIL: 找不到 127.0.0.2:%d 的 listener fd\n", PROXY_PORT);
                failed = 1;
            } else {
                unsigned long long ino_before = fd_socket_inode(lfd);
                close(lfd); // 模擬核心所為：fd 失效但引擎不知情
                // 等對帳週期跑幾輪（poll 逾時 1 秒）。此時 127.0.0.2 仍在 desired
                // 集合內，所以引擎唯一的出路就是「發現失效 → 重建」。
                usleep(3000 * 1000);
                int newfd = find_listener_fd("127.0.0.2", PROXY_PORT);
                unsigned long long ino_after = (newfd >= 0) ? fd_socket_inode(newfd) : 0;
                if (newfd < 0 || ino_after == 0 || ino_after == ino_before) {
                    fprintf(stderr, "FAIL: listener 被關掉後引擎沒有重建"
                                    "（inode %llu → %llu，代理會顯示運行中卻不通）\n",
                            ino_before, ino_after);
                    failed = 1;
                } else {
                    // 真正的可用性判據：完整 SOCKS5 握手 + CONNECT + 資料回流。
                    // bare connect() 不算數 —— 舊 socket 在 poll() 持有引用的期間
                    // 仍會把連線收進 backlog，連得上卻沒有人服務。
                    int c_heal = connect_to("127.0.0.2", PROXY_PORT);
                    int ok = (c_heal >= 0
                              && proxy_open_tunnel(c_heal, "127.0.0.1", ECHO_PORT) == 0
                              && echo_on_fd(c_heal, "healed", 6) == 0);
                    if (c_heal >= 0) close(c_heal);
                    if (!ok) {
                        fprintf(stderr, "FAIL: 重建後的 listener 無法服務連線\n");
                        failed = 1;
                    } else {
                        printf("PASS: listener 被關掉後引擎自行重建並可服務（inode %llu → %llu）\n",
                               ino_before, ino_after);
                    }
                }
            }
        }

        // (e) 從 desired 集合移除的位址必須下線（不再佔用槽位，也避免無限累積）
        const char *addrs_back[] = { "127.0.0.1" };
        socks5_server_set_bind_addrs(addrs_back, 1);
        usleep(2500 * 1000);
        int c_old = connect_to("127.0.0.2", PROXY_PORT);
        if (c_old >= 0) {
            fprintf(stderr, "FAIL: 已從 desired 移除的位址仍在監聽\n");
            failed = 1;
            close(c_old);
        } else {
            printf("PASS: 移除的綁定位址已下線\n");
        }

        // (f) 綁不上的位址不得把整個服務帶走（優雅降級）：192.0.2.0/24 是 TEST-NET-1，
        //     本機不存在，bind 必然失敗；loopback 必須照常服務。
        const char *addrs_bad[] = { "192.0.2.7" };
        socks5_server_set_bind_addrs(addrs_bad, 1);
        usleep(2500 * 1000);
        const char *alive = "still-alive";
        if (do_roundtrip(alive, (int)strlen(alive)) != 0) {
            fprintf(stderr, "FAIL: 無法綁定的位址把 loopback 服務也帶走了\n");
            failed = 1;
        } else {
            printf("PASS: 無法綁定的位址不影響既有服務\n");
        }
        socks5_server_set_bind_addrs(addrs_back, 1); // 還原

        if (have_keep) close(keep);
    }

    // 7. 停止
    socks5_server_quit();
    g_echo_stop = 1;
    pthread_join(echo_tid, NULL);
    pthread_join(udp_echo_tid, NULL);

    if (failed) {
        fprintf(stderr, "RESULT: FAIL\n");
        return 1;
    }
    printf("RESULT: ALL PASS\n");
    return 0;
}
