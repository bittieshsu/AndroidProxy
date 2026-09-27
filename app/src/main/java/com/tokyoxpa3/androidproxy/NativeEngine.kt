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

    // [listener 重建] 安全更新綁定位址集合。失敗只記一筆 warning —— 位址集合更新
    // 失敗不該把代理帶走：引擎仍會用既有的集合繼續服務，下次介面事件會再試。
    fun safeUpdateBindAddrs(bindAddrs: Array<String>) {
        if (!libraryLoaded) return
        try {
            updateSocks5BindAddrs(bindAddrs)
        } catch (e: Exception) {
            Log.w(TAG, "updateSocks5BindAddrs 失敗: ${e.message}")
        }
    }

    fun notifySocketClosed(fd: Int) {
        onSocketClosed?.invoke(fd)
    }
    
    external fun startSocks5Server(port: Int, bindAddrs: Array<String>): String
    // [listener 重建] 執行期更新監聽位址集合（Wi-Fi／熱點／USB 分享位址變動時）。
    // 只更新「想要的集合」，實際 listener 由引擎在 ≤1 秒內對齊：新位址綁上、
    // 消失的位址下線，**不重建代理、不斷既有連線**。這是「介面被關掉時核心會把
    // listener 一起關掉，而舊版永不重建」的修復入口。
    external fun updateSocks5BindAddrs(bindAddrs: Array<String>): String
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

    // [拆除診斷] UDP session 拆除的分類計數（total/short/idle/peer/io/proto/
    // resource/shutdown/unknown/verbose）。永遠開啟，成本是每個 session 一次
    // atomic 加 —— 所以「不開 log 也看得見 churn」。
    external fun getUdpCloseStats(): String
    // [開發者開關] 逐行拆除 log。預設關：一般使用者不需要，開了只會把 logcat 沖掉。
    // 執行期可切換（不必重啟 server）。漏插樁警告不受此開關管制，永遠出聲。
    external fun setUdpVerboseLog(on: Boolean)

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

    // [拆除診斷] 安全讀取 UDP 拆除分類計數；程式庫未載入時回傳說明字串。
    fun safeGetUdpCloseStats(): String {
        return if (libraryLoaded) {
            try { getUdpCloseStats() } catch (e: Exception) { "udp close stats unavailable" }
        } else {
            "native library not loaded"
        }
    }

    // [開發者開關] 安全套用逐行拆除 log 開關。失敗只記一筆 warning —— 這是診斷
    // 功能，不該因為它而影響代理運作。
    fun safeSetUdpVerboseLog(on: Boolean) {
        if (!libraryLoaded) return
        try { setUdpVerboseLog(on) } catch (e: Exception) {
            Log.w(TAG, "setUdpVerboseLog($on) 失敗: ${e.message}")
        }
    }
}
