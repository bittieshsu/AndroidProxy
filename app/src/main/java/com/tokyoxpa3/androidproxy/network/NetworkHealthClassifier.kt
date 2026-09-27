package com.tokyoxpa3.androidproxy.network

/**
 * 5G 出口的健康狀態。
 *
 * [BLOCKED_BY_VPN] 與 [NETWORK_DEAD] 的差別**只在「要不要自動重建」**：
 * 被 VPN 納入隧道是設定問題，重建一萬次也不會好，而且會把既有連線整批切斷；
 * 網路真的斷了則重建有用。所以誤判的方向決定代價 —— 把真斷線判成
 * BLOCKED_BY_VPN 會讓代理永遠躺在死掉的狀態，比多重建幾次嚴重得多。
 */
enum class NetworkHealthStatus { OK, BLOCKED_BY_VPN, NETWORK_DEAD }

/**
 * 健康檢查的三態判定（純函式，不依賴 Android，可用 JVM 單元測試）。
 *
 * 判定順序刻意如此：
 *
 * 1. **網路本體沒了**（caps == null 或失去 INTERNET）→ [NetworkHealthStatus.NETWORK_DEAD]。
 *    這與 VPN 無關：VPN 是在蜂巢式之上再疊一層虛擬網路，不會讓底層的 Network
 *    物件消失或失去 INTERNET capability。舊版「caps 異常就怪 VPN」的推論是錯的，
 *    實測症狀是：Client 的 VPN 一開，5G 真的斷線也會被判成 BLOCKED_BY_VPN，
 *    於是永遠不重建、只能手動重啟。
 *
 * 2. **bindSocket 探測** —— 這才是 VPN 真正會破壞的那一步
 *    （實機 log: `Binding socket to network 176 failed: EPERM`）。
 *    bind 失敗 + VPN 運作中 + 蜂巢式仍被系統驗證可用（VALIDATED）
 *    → [NetworkHealthStatus.BLOCKED_BY_VPN]，其餘一律 NETWORK_DEAD。
 *    `cellularValidated` 這個附加條件是必要的：只憑「有沒有 VPN」判斷，會把真實
 *    斷線誤判成 VPN 阻擋；而蜂巢式若連系統驗證都過不了，不管有沒有 VPN 都該重建。
 *
 * 3. **bind 過得去** → 出口是通的，此時探測端點連不上就是上游真的壞了 → NETWORK_DEAD。
 *    舊版沒有這一步，於是被 VPN 納入（但其實沒被擋）的情況下，端點全不通也會被
 *    歸咎給 VPN 而不重建。
 *
 * 注意：這裡判的是「**本 App 能不能用蜂巢式出口**」，不是「系統裡有沒有 VPN」。
 * 兩者差很多 —— per-app VPN（split tunneling）沒包含本 App 時，VPN 存在但完全不影響。
 */
object NetworkHealthClassifier {

    fun classify(
        hasInternetCapability: Boolean,
        cellularValidated: Boolean,
        bindSucceeded: Boolean,
        vpnActive: Boolean,
        probeSucceeded: Boolean
    ): NetworkHealthStatus {
        if (!hasInternetCapability) return NetworkHealthStatus.NETWORK_DEAD

        if (!bindSucceeded) {
            return if (vpnActive && cellularValidated) {
                NetworkHealthStatus.BLOCKED_BY_VPN
            } else {
                NetworkHealthStatus.NETWORK_DEAD
            }
        }

        return if (probeSucceeded) NetworkHealthStatus.OK else NetworkHealthStatus.NETWORK_DEAD
    }
}
