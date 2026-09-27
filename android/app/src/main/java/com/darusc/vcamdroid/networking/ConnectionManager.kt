package com.darusc.vcamdroid.networking

import com.darusc.vcamdroid.networking.connection.Connection
import com.darusc.vcamdroid.networking.connection.TCPConnection
import com.darusc.vcamdroid.util.Logger
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.launch

class ConnectionManager private constructor() : Connection.Listener {

    companion object {
        @Volatile private var instance: ConnectionManager? = null
        fun getInstance(): ConnectionManager = synchronized(this) {
            instance ?: ConnectionManager().also { instance = it }
        }
        fun getInstance(connectionStateCallback: ConnectionStateCallback): ConnectionManager =
            getInstance().also { it.setConnectionStateCallback(connectionStateCallback) }
    }

    enum class ConnectionMode { WIFI, USB }

    interface ConnectionStateCallback {
        fun onConnectionSuccessful(connectionMode: ConnectionMode) {}
        fun onConnectionFailed(connectionMode: ConnectionMode) {}
        fun onDisconnected() {}
    }

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private val lock = Any()
    private var connectionStateCallback: ConnectionStateCallback? = null
    private var onFrameReceivedCallback: ((type: Int, payload: ByteArray) -> Unit)? = null
    private var onLegacyBytesReceivedCallback: ((buffer: ByteArray, bytes: Int) -> Unit)? = null
    private var tcpConn: TCPConnection? = null
    private var connectGeneration = 0L

    val localIpAddress: String
        get() = synchronized(lock) { requireNotNull(tcpConn) { "Not connected" }.localIpAddress }

    fun mediaPassword(): String? = synchronized(lock) { tcpConn?.mediaPassword() }
    fun isAuthenticated(): Boolean = synchronized(lock) { tcpConn?.isAuthenticated == true }

    private fun setConnectionStateCallback(callback: ConnectionStateCallback) {
        connectionStateCallback = callback
    }

    fun setOnFrameReceivedCallback(callback: (type: Int, payload: ByteArray) -> Unit) {
        onFrameReceivedCallback = callback
    }

    fun setOnLegacyBytesReceivedCallback(callback: (buffer: ByteArray, bytes: Int) -> Unit) {
        onLegacyBytesReceivedCallback = callback
    }

    fun connect(ipAddress: String, port: Int, pairingToken: String) {
        connectInternal(ipAddress, port, false, pairingToken, ConnectionMode.WIFI)
    }

    /** USB legacy mode is protected by the user's authorized ADB transport. */
    fun connect(port: Int) {
        connectInternal("127.0.0.1", port, true, null, ConnectionMode.USB)
    }

    private fun connectInternal(ip: String, port: Int, adb: Boolean, token: String?, mode: ConnectionMode) {
        val generation = synchronized(lock) {
            connectGeneration++
            tcpConn?.close()
            tcpConn = null
            connectGeneration
        }
        scope.launch {
            try {
                val next = TCPConnection(ip, port, adb, this@ConnectionManager, token)
                val accepted = synchronized(lock) {
                    if (generation != connectGeneration) false else {
                        tcpConn = next
                        true
                    }
                }
                if (accepted) {
                    next.startReceiver()
                    connectionStateCallback?.onConnectionSuccessful(mode)
                } else next.close()
            } catch (e: Connection.ConnectionFailedException) {
                Logger.log("CONNECTION MANAGER", "Connection/authentication failed")
                if (synchronized(lock) { generation == connectGeneration })
                    connectionStateCallback?.onConnectionFailed(mode)
            }
        }
    }

    fun sendDescriptor(descriptor: DeviceDescriptor) {
        scope.launch {
            try {
                val connection = synchronized(lock) { tcpConn }
                    ?: throw IllegalStateException("No active control connection")
                connection.sendMessage(ControlProtocol.TYPE_DESCRIPTOR, descriptor.serialize())
                Logger.log("CONNECTION MANAGER", "Device descriptor sent")
            } catch (e: Exception) {
                Logger.log("CONNECTION MANAGER", "Descriptor send failed (${e.javaClass.simpleName})")
                synchronized(lock) { tcpConn }?.let { onDisconnected(it) }
            }
        }
    }

    fun sendErrorReport(report: ErrorReport) {
        scope.launch {
            try {
                synchronized(lock) { tcpConn }?.sendMessage(
                    ControlProtocol.TYPE_ERROR_REPORT, report.serialize())
            } catch (_: Exception) {
                synchronized(lock) { tcpConn }?.let { onDisconnected(it) }
            }
        }
    }

    override fun onFrameReceived(connection: TCPConnection, type: Int, payload: ByteArray) {
        if (synchronized(lock) { tcpConn !== connection }) return
        onFrameReceivedCallback?.invoke(type, payload)
    }

    override fun onLegacyBytes(connection: TCPConnection, buffer: ByteArray, bytes: Int) {
        if (synchronized(lock) { tcpConn !== connection }) return
        onLegacyBytesReceivedCallback?.invoke(buffer, bytes)
    }

    override fun onDisconnected(connection: TCPConnection) {
        scope.launch(Dispatchers.Main) {
            val current = synchronized(lock) {
                if (tcpConn !== connection) false else { tcpConn = null; true }
            }
            if (!current) return@launch
            connection.close()
            connectionStateCallback?.onDisconnected()
        }
    }

    fun close() {
        val old = synchronized(lock) {
            connectGeneration++
            tcpConn.also { tcpConn = null }
        }
        old?.close()
    }
}
