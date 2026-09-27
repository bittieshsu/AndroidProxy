package com.tokyoxpa3.androidproxy.network

import org.junit.Assert.assertFalse
import org.junit.Test

class VpnDetectorTest {

    @Test
    fun nullConnectivityManagerReturnsFalse() {
        assertFalse(VpnDetector.isVpnActive(null))
    }
}
