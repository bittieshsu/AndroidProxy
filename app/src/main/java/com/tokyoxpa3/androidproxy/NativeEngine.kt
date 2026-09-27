package com.tokyoxpa3.androidproxy

import android.util.Log

object NativeEngine {
    private const val TAG = "NativeEngine"
    @Volatile private var libraryLoaded = false
    @Volatile private var initialized = false
    
    // 由 Java 執行緒寫入、native worker 執行緒讀取（createSocketFromNative /
    // notifySocketClosed）：必須 @Volatile 保證跨執行緒可見性，否則重建後 native 端
    // 可能仍看到舊的 provider，或呼叫到已 teardown 的 callback。
    @Volatile var socketProvider: ((String, Int, Boolean) -> Int)? = null
    @Volatile var onSocketClosed: ((Int) -> Unit)? = null
    // [UDP 網域 frame] UDP relay 的 ATYP=0x03 需要「以蜂巢式網路」解析主機名。
    // 由 native UDP worker 執行緒呼叫，因此與上面兩個一樣必須 @Volatile。
    // 回傳 IP 字面值（IPv4/IPv6），無法解析時回 null。
    @Volatile var hostResolver: ((String) -> String?)? = null

    init {
        try {
            Log.d(TAG, "Loading native library: androidproxy")
            System.loadLibrary("androidproxy")
            libraryLoaded = true
            Log.d(TAG, "✅ Native library loaded successfully")
        } catch (e: Exception) {
            Log.e(TAG, "❌ Failed to load native library: ${e.message}")
            libraryLoaded = false
        }
    }
    
    fun isLibraryLoaded(): Boolean = libraryLoaded
    
    fun registerInstance() {
        if (!initialized && socketProvider != null) {
            Log.d(TAG, "Registering NativeEngine instance with C++ layer")
            nativeRegisterInstance()
            initialized = true
        }
    }
    
    private external fun nativeRegisterInstance()
    
    fun createSocketFromNative(host: String, port: Int, isUdp: Boolean): Int {
        return socketProvider?.invoke(host, port, isUdp) ?: -1
    }

    /**
     * 供 native UDP worker 呼叫（ATYP=0x03 網域 frame）。
     * 任何例外都吞掉並回 null —— native 端只會把該 frame 丟棄，
     * 不該讓解析問題把 UDP worker 帶走。
     */
    fun resolveHostFromNative(host: String): String? {
        return try {
            hostResolver?.invoke(host)
        } catch (e: Exception) {
            Log.w(TAG, "resolveHostFromNative($host) 失敗: ${e.message}")
            null
        }
    }

    fun notifySocketClosed(fd: Int) {
        onSocketClosed?.invoke(fd)
    }
    
    external fun startSocks5Server(port: Int, bindAddrs: Array<String>): String
    external fun stopSocks5Server(): String
    external fun setSocks5Auth(user: String, pass: String): String
    external fun isSocks5ServerRunning(): Boolean
    external fun getSocks5Stats(): String
    external fun getTrafficBytes(): LongArray
    // [非同步解析診斷] UDP relay 的網域解析計數
    // （enq/ok/fail/replay/drop/qfull/jobfull/lost）。
    // 舊版解析失敗是完全靜默的：客戶端送出的 datagram 永遠等不到回覆，log 裡卻
    // 一行線索都沒有 —— 這正是「驗證頁卡住卻查不出原因」的來源。這個計數補上該缺口。
    external fun getDnsStats(): String

    // [自檢/診斷] 安全讀取 native 統計；程式庫未載入時回傳說明字串
    fun safeGetStats(): String {
        return if (libraryLoaded) {
            try { getSocks5Stats() } catch (e: Exception) { "stats unavailable" }
        } else {
            "native library not loaded"
        }
    }

    // [流量統計] 安全讀取 tx/rx 累計位元組（[上傳, 下載]）；程式庫未載入或
    // 讀取失敗回傳 null，由 UI 顯示佔位。
    fun safeGetTrafficBytes(): LongArray? {
        return if (libraryLoaded) {
            try {
                val arr = getTrafficBytes()
                if (arr.size >= 2) arr else null
            } catch (e: Exception) { null }
        } else null
    }

    // [非同步解析診斷] 安全讀取 UDP 網域解析計數；程式庫未載入時回傳說明字串。
    fun safeGetDnsStats(): String {
        return if (libraryLoaded) {
            try { getDnsStats() } catch (e: Exception) { "dns stats unavailable" }
        } else {
            "native library not loaded"
        }
    }
}
