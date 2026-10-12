package com.iclforge.hearth

import android.content.Context
import android.net.nsd.NsdManager
import android.net.nsd.NsdServiceInfo
import android.util.Log

/**
 * Advertises the sink as `_sendspin._tcp` through the platform's [NsdManager]
 * (planning/hearth-android.md, "Discovery, pairing, connection"). The listening socket and the
 * WebSocket are the native library's; this is the half only Android has, as the board's firmware
 * has its own mDNS around the same library.
 *
 * The TXT records are the ones a Sendspin player gives: `path` (the WebSocket's) and `name`.
 */
class SinkAdvertiser(context: Context) {
    private val manager = context.getSystemService(Context.NSD_SERVICE) as NsdManager
    private var listener: NsdManager.RegistrationListener? = null

    /** The name the platform registered the service under, once it has: it may differ from the one asked for. */
    @Volatile
    var registeredName: String? = null
        private set

    @Volatile
    var failure: String? = null
        private set

    fun register(name: String, port: Int) {
        unregister()
        val info = NsdServiceInfo().apply {
            serviceName = name
            serviceType = SERVICE_TYPE
            setPort(port)
            setAttribute("path", PATH)
            setAttribute("name", name)
        }
        val created = object : NsdManager.RegistrationListener {
            override fun onServiceRegistered(registered: NsdServiceInfo) {
                registeredName = registered.serviceName
                failure = null
                Log.i(TAG, "advertised ${registered.serviceName} on port $port")
            }

            override fun onRegistrationFailed(service: NsdServiceInfo, errorCode: Int) {
                failure = "NsdManager registration failed ($errorCode)"
                Log.e(TAG, failure!!)
            }

            override fun onServiceUnregistered(service: NsdServiceInfo) {
                registeredName = null
                Log.i(TAG, "no longer advertised: ${service.serviceName}")
            }

            override fun onUnregistrationFailed(service: NsdServiceInfo, errorCode: Int) {
                Log.e(TAG, "NsdManager unregistration failed ($errorCode)")
            }
        }
        listener = created
        manager.registerService(info, NsdManager.PROTOCOL_DNS_SD, created)
    }

    fun unregister() {
        val current = listener ?: return
        listener = null
        try {
            manager.unregisterService(current)
        } catch (e: IllegalArgumentException) {
            // Never registered, or already gone.
            Log.w(TAG, "unregisterService: ${e.message}")
        }
    }

    companion object {
        private const val TAG = "HearthSink"
        const val SERVICE_TYPE = "_sendspin._tcp."
        const val PATH = "/sendspin"
    }
}
