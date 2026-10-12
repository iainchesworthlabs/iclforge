package com.iclforge.hearth

import android.content.Context
import android.net.ConnectivityManager
import android.net.NetworkCapabilities
import java.net.Inet4Address

/**
 * Where the box is on the network, for the idle screen (planning/hearth-sink-screen.md, S1): its IPv4
 * address on the active network and what kind of network that is. The name of a Wi-Fi network needs a
 * location permission, which a sink has no other use for, so the kind is what is said.
 */
object NetworkInfo {
    data class Where(val address: String, val kind: String?)

    fun current(context: Context): Where? {
        val manager = context.getSystemService(ConnectivityManager::class.java) ?: return null
        val network = manager.activeNetwork ?: return null
        val properties = manager.getLinkProperties(network) ?: return null
        val address = properties.linkAddresses
            .map { it.address }
            .filterIsInstance<Inet4Address>()
            .firstOrNull { !it.isLoopbackAddress && !it.isLinkLocalAddress }
            ?.hostAddress
            ?: return null
        val capabilities = manager.getNetworkCapabilities(network)
        val kind = when {
            capabilities == null -> null
            capabilities.hasTransport(NetworkCapabilities.TRANSPORT_ETHERNET) -> "Ethernet"
            capabilities.hasTransport(NetworkCapabilities.TRANSPORT_WIFI) -> "Wi-Fi"
            else -> null
        }
        return Where(address, kind)
    }
}
