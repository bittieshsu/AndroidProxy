#ifndef JNI_BRIDGE_H
#define JNI_BRIDGE_H

#include <jni.h>

#ifdef __cplusplus
extern "C" {
#endif

extern JavaVM *g_jvm;

// 修正後的簽名：增加 is_udp
int request_java_5g_socket(const char *host, int port, int is_udp);
// 以蜂巢式網路解析主機名（供 UDP relay 的 ATYP=0x03 網域 frame 使用）。
// 成功回 0 並把 IP 字面值（IPv4 或 IPv6）寫進 out；失敗回 -1（out 會被清空）。
int request_java_resolve_host(const char *host, char *out, size_t out_len);
void release_java_socket(int fd);

#ifdef __cplusplus
}
#endif

#endif // JNI_BRIDGE_H