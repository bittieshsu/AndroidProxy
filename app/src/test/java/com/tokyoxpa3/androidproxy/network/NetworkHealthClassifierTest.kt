package com.tokyoxpa3.androidproxy.network

import org.junit.Assert.assertEquals
import org.junit.Test

class NetworkHealthClassifierTest {

    private fun classify(
        hasInternet: Boolean = true,
        validated: Boolean = true,
        bindOk: Boolean = true,
        vpnActive: Boolean = false,
        probeOk: Boolean = true
    ) = NetworkHealthClassifier.classify(
        hasInternetCapability = hasInternet,
        cellularValidated = validated,
        bindSucceeded = bindOk,
        vpnActive = vpnActive,
        probeSucceeded = probeOk
    )

    // --- 正常路徑 ---

    @Test
    fun healthyPathIsOk() {
        assertEquals(NetworkHealthStatus.OK, classify())
    }

    // --- 被 VPN 阻擋：只有「bind 失敗 + 有 VPN + 蜂巢式仍被系統驗證可用」才算 ---

    @Test
    fun bindDeniedWithActiveValidatedVpnIsBlocked() {
        assertEquals(
            NetworkHealthStatus.BLOCKED_BY_VPN,
            classify(bindOk = false, vpnActive = true, validated = true, probeOk = false)
        )
    }

    @Test
    fun bindDeniedWithoutVpnIsNetworkDead() {
        assertEquals(
            NetworkHealthStatus.NETWORK_DEAD,
            classify(bindOk = false, vpnActive = false, probeOk = false)
        )
    }

    // --- 回歸：這些以前會被誤判成 BLOCKED_BY_VPN，於是永不自動重建 ---

    /** 蜂巢式已失去 INTERNET capability：與 VPN 無關，就是斷線。 */
    @Test
    fun lostInternetCapabilityIsNetworkDeadEvenWithVpn() {
        assertEquals(
            NetworkHealthStatus.NETWORK_DEAD,
            classify(hasInternet = false, bindOk = false, vpnActive = true)
        )
    }

    /**
     * 蜂巢式連系統驗證都過不了（VALIDATED 為 false）：不管有沒有 VPN，
     * 都該走 NETWORK_DEAD 的自動重建路徑，不能因為「有 VPN」就放棄重建。
     */
    @Test
    fun bindDeniedWithUnvalidatedCellularIsNetworkDeadEvenWithVpn() {
        assertEquals(
            NetworkHealthStatus.NETWORK_DEAD,
            classify(bindOk = false, vpnActive = true, validated = false, probeOk = false)
        )
    }

    /** bind 過得去 → 出口可用，探測端點全失敗代表上游真的壞了，必須可重建。 */
    @Test
    fun bindOkButProbesFailedIsNetworkDeadEvenWithVpn() {
        assertEquals(
            NetworkHealthStatus.NETWORK_DEAD,
            classify(bindOk = true, vpnActive = true, probeOk = false)
        )
    }
}
