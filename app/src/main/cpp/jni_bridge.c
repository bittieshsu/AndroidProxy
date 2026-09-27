#include "jni_bridge.h"
#include <jni.h>
#include <android/log.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>

#define TAG "JNI_BRIDGE"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

JavaVM *g_jvm = NULL;
jobject g_native_engine_instance = NULL;

// 快取 MethodID 避免反覆查詢 (效能關鍵)
static jmethodID g_mid_createSocket = NULL;
static jmethodID g_mid_notifyClosed = NULL;
static jmethodID g_mid_resolveHost = NULL;

extern int socks5_server_main_dynamic(int port);
extern void socks5_server_quit(void);
extern void socks5_server_set_auth(const char *user, const char *pass);
extern void socks5_server_set_bind_addrs(const char **addrs, int count);
extern int socks5_server_is_running(void);
extern int socks5_server_get_stats(char *out, size_t out_len);
extern int socks5_server_get_bytes(long long *tx, long long *rx);
// [非同步解析] UDP relay 的網域解析診斷計數（enq/ok/fail/replay/drop/qfull/...）。
// 舊版解析失敗完全靜默，這個計數就是補上那個缺口，讓「驗證頁卡住」能在 log 裡現形。
extern int socks5_server_get_dns_stats(char *out, size_t out_len);

static pthread_t g_server_thread;
static int g_server_running = 0;

typedef struct { int port; } ServerArgs;

static void *server_thread_func(void *arg) {
    ServerArgs *args = (ServerArgs *)arg;
    if (socks5_server_main_dynamic(args->port) != 0) g_server_running = 0;
    free(args);
    return NULL;
}

// 供 Worker 線程使用：永久綁定 JVM
void jni_attach_thread() {
    if (!g_jvm) return;
    JNIEnv *env;
    (*g_jvm)->AttachCurrentThread(g_jvm, &env, NULL);
}

void jni_detach_thread() {
    if (!g_jvm) return;
    (*g_jvm)->DetachCurrentThread(g_jvm);
}

static JNIEnv *get_jni_env(int *should_detach) {
    JNIEnv *env = NULL;
    *should_detach = 0;
    if (!g_jvm) return NULL;
    int res = (*g_jvm)->GetEnv(g_jvm, (void **)&env, JNI_VERSION_1_6);
    if (res == JNI_EDETACHED) {
        if ((*g_jvm)->AttachCurrentThread(g_jvm, &env, NULL) != 0) return NULL;
        *should_detach = 1;
    }
    return env;
}

int request_java_5g_socket(const char *host, int port, int is_udp) {
    int should_detach = 0;
    JNIEnv *env = get_jni_env(&should_detach);
    if (!env || !g_native_engine_instance || !g_mid_createSocket) return -1;

    jstring jhost = (*env)->NewStringUTF(env, host);
    jint fd = (*env)->CallIntMethod(env, g_native_engine_instance, g_mid_createSocket, jhost, (jint)port, (jboolean)is_udp);

    // 異常防護
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        fd = -1; 
    }

    (*env)->DeleteLocalRef(env, jhost);
    if (should_detach) (*g_jvm)->DetachCurrentThread(g_jvm);
    return (int)fd;
}

// 以蜂巢式網路解析主機名（Java 端 resolveWithCache：IP 字面值快路徑 + 快取 +
// DnsResolver 逾時）。供 UDP relay 的 ATYP=0x03 網域 frame 使用——UDP worker
// 若自己呼叫 getaddrinfo，會走系統預設網路（在 Pro 手機上可能繞回 VPN）。
// 成功回 0 並把 IP 字面值寫進 out；失敗回 -1（out 清空）。
int request_java_resolve_host(const char *host, char *out, size_t out_len) {
    if (!out || out_len == 0) return -1;
    out[0] = '\0';
    int should_detach = 0;
    JNIEnv *env = get_jni_env(&should_detach);
    if (!env || !g_native_engine_instance || !g_mid_resolveHost) return -1;

    jstring jhost = (*env)->NewStringUTF(env, host);
    if (!jhost) {
        if (should_detach) (*g_jvm)->DetachCurrentThread(g_jvm);
        return -1;
    }
    jstring jres = (jstring)(*env)->CallObjectMethod(env, g_native_engine_instance, g_mid_resolveHost, jhost);

    int rc = -1;
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env); // 解析失敗在 Java 端是回 null，這裡只防禦意外例外
    } else if (jres) {
        const char *s = (*env)->GetStringUTFChars(env, jres, NULL);
        if (s) {
            strncpy(out, s, out_len - 1);
            out[out_len - 1] = '\0';
            (*env)->ReleaseStringUTFChars(env, jres, s);
            if (out[0]) rc = 0;
        }
    }
    if (jres) (*env)->DeleteLocalRef(env, jres);
    (*env)->DeleteLocalRef(env, jhost);
    if (should_detach) (*g_jvm)->DetachCurrentThread(g_jvm);
    return rc;
}

void release_java_socket(int fd) {
    int should_detach = 0;
    JNIEnv *env = get_jni_env(&should_detach);
    if (!env || !g_native_engine_instance || !g_mid_notifyClosed) return;
    
    (*env)->CallVoidMethod(env, g_native_engine_instance, g_mid_notifyClosed, (jint)fd);
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);

    if (should_detach) (*g_jvm)->DetachCurrentThread(g_jvm);
}

JNIEXPORT void JNICALL native_register_instance(JNIEnv *env, jobject thiz) {
    (*env)->GetJavaVM(env, &g_jvm);
    // [執行緒安全] registerInstance 只在服務首次啟動時呼叫一次（NativeEngine 的
    // initialized 旗標），且註冊發生在任何 worker 執行緒建立之前。若萬一被重複
    // 呼叫，這裡直接返回，避免在 worker 正透過 request_java_5g_socket 使用舊
    // global ref / MethodID 時把它們刪除替換（會是 use-after-free）。
    if (g_native_engine_instance != NULL && g_mid_createSocket != NULL && g_mid_notifyClosed != NULL && g_mid_resolveHost != NULL) {
        return;
    }
    if (g_native_engine_instance) (*env)->DeleteGlobalRef(env, g_native_engine_instance);
    g_native_engine_instance = (*env)->NewGlobalRef(env, thiz);

    // 初始化 MethodID 快取
    jclass cls = (*env)->GetObjectClass(env, thiz);
    g_mid_createSocket = (*env)->GetMethodID(env, cls, "createSocketFromNative", "(Ljava/lang/String;IZ)I");
    g_mid_notifyClosed = (*env)->GetMethodID(env, cls, "notifySocketClosed", "(I)V");
    g_mid_resolveHost = (*env)->GetMethodID(env, cls, "resolveHostFromNative", "(Ljava/lang/String;)Ljava/lang/String;");
    // GetMethodID 找不到方法時會丟 NoSuchMethodError 並回 NULL；清掉例外，
    // 讓「解析器不可用」只表現為 ATYP=0x03 frame 解析失敗，而不是整個服務崩掉。
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
}

JNIEXPORT jstring JNICALL native_start_socks5_server(JNIEnv *env, jobject thiz, jint port, jobjectArray jAddrs) {
    if (g_server_running) return (*env)->NewStringUTF(env, "Already running");

    // 從 Java 複製綁定位址到 C 層靜態緩衝區（socks5_server_set_bind_addrs 會自行複製內容）
    const char *tmp[32];
    jstring jsArr[32];
    int count = 0;
    if (jAddrs) {
        jsize len = (*env)->GetArrayLength(env, jAddrs);
        if (len > 32) len = 32;
        for (jsize i = 0; i < len; i++) {
            jstring js = (jstring)(*env)->GetObjectArrayElement(env, jAddrs, i);
            if (!js) continue;
            const char *c = (*env)->GetStringUTFChars(env, js, NULL);
            if (c) {
                jsArr[count] = js;
                tmp[count] = c;
                count++;
            } else {
                (*env)->DeleteLocalRef(env, js);
            }
        }
    }
    socks5_server_set_bind_addrs(tmp, count);
    for (int i = 0; i < count; i++) {
        (*env)->ReleaseStringUTFChars(env, jsArr[i], tmp[i]);
        (*env)->DeleteLocalRef(env, jsArr[i]);
    }

    ServerArgs *args = malloc(sizeof(ServerArgs));
    args->port = (int)port;
    g_server_running = 1;
    pthread_create(&g_server_thread, NULL, server_thread_func, args);
    return (*env)->NewStringUTF(env, "Started");
}

JNIEXPORT jstring JNICALL native_stop_socks5_server(JNIEnv *env, jobject thiz) {
    if (!g_server_running) return (*env)->NewStringUTF(env, "Not running");
    socks5_server_quit();
    pthread_join(g_server_thread, NULL);
    g_server_running = 0;
    return (*env)->NewStringUTF(env, "Stopped");
}

JNIEXPORT jstring JNICALL native_set_socks5_auth(JNIEnv *env, jobject thiz, jstring user, jstring pass) {
    const char *cuser = user ? (*env)->GetStringUTFChars(env, user, NULL) : NULL;
    const char *cpass = pass ? (*env)->GetStringUTFChars(env, pass, NULL) : NULL;
    socks5_server_set_auth(cuser ? cuser : "", cpass ? cpass : "");
    if (cuser) (*env)->ReleaseStringUTFChars(env, user, cuser);
    if (cpass) (*env)->ReleaseStringUTFChars(env, pass, cpass);
    return (*env)->NewStringUTF(env, "OK");
}

// [item6] 零成本健康檢查：直接讀取 C 層的 atomic 運行旗標
JNIEXPORT jboolean JNICALL native_is_socks5_server_running(JNIEnv *env, jobject thiz) {
    return (jboolean)(socks5_server_is_running() ? 1 : 0);
}

// [自檢/診斷] 讀取 native 引擎即時統計（App 內「複製診斷報告」用）
JNIEXPORT jstring JNICALL native_get_socks5_stats(JNIEnv *env, jobject thiz) {
    char buf[512];
    socks5_server_get_stats(buf, sizeof(buf));
    return (*env)->NewStringUTF(env, buf);
}

// [流量統計] 讀取 tx/rx 累計位元組（[上傳, 下載]），供 UI 即時速率顯示。
JNIEXPORT jlongArray JNICALL native_get_traffic_bytes(JNIEnv *env, jobject thiz) {
    long long tx = 0, rx = 0;
    socks5_server_get_bytes(&tx, &rx);
    jlongArray arr = (*env)->NewLongArray(env, 2);
    if (arr) {
        jlong vals[2] = { (jlong)tx, (jlong)rx };
        (*env)->SetLongArrayRegion(env, arr, 0, 2, vals);
    }
    return arr;
}

// [非同步解析診斷] UDP relay 網域解析計數（App 內「複製診斷報告」用）。
JNIEXPORT jstring JNICALL native_get_dns_stats(JNIEnv *env, jobject thiz) {
    char buf[256];
    socks5_server_get_dns_stats(buf, sizeof(buf));
    return (*env)->NewStringUTF(env, buf);
}

static const JNINativeMethod gMethods[] = {
    {"nativeRegisterInstance", "()V", (void *)native_register_instance},
    {"startSocks5Server", "(I[Ljava/lang/String;)Ljava/lang/String;", (void *)native_start_socks5_server},
    {"stopSocks5Server", "()Ljava/lang/String;", (void *)native_stop_socks5_server},
    {"setSocks5Auth", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;", (void *)native_set_socks5_auth},
    {"isSocks5ServerRunning", "()Z", (void *)native_is_socks5_server_running},
    {"getSocks5Stats", "()Ljava/lang/String;", (void *)native_get_socks5_stats},
    {"getTrafficBytes", "()[J", (void *)native_get_traffic_bytes},
    {"getDnsStats", "()Ljava/lang/String;", (void *)native_get_dns_stats},
};

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
    g_jvm = vm;
    JNIEnv *env;
    if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
    jclass cls = (*env)->FindClass(env, "com/tokyoxpa3/androidproxy/NativeEngine");
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); return JNI_ERR; }
    (*env)->RegisterNatives(env, cls, gMethods, sizeof(gMethods) / sizeof(gMethods[0]));
    return JNI_VERSION_1_6;
}