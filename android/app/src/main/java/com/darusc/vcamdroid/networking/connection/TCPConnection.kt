package com.darusc.vcamdroid.networking.connection

import com.darusc.vcamdroid.networking.ControlProtocol
import com.darusc.vcamdroid.util.Logger
import java.io.DataInputStream
import java.net.InetSocketAddress
import java.net.Socket
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.atomic.AtomicBoolean

/** A framed/authenticated Wi-Fi connection, or the legacy ADB-only Windows transport. */
class TCPConnection(
    ipAddress: String,
    port: Int,
    private val isOverAdb: Boolean,
    private val listener: Connection.Listener,
    pairingTokenHex: String? = null
) : Connection() {

    override val maxPacketSize = ControlProtocol.MAX_PAYLOAD
    private val socket = Socket()
    private val running = AtomicBoolean(true)
    private val authenticatedChannel: ControlProtocol.AuthenticatedChannel?
    private lateinit var thread: Thread

    override val localIpAddress: String
        get() = socket.localAddress.hostAddress ?: error("No local address")

    fun mediaPassword(): String? = authenticatedChannel?.mediaPassword()
    val isAuthenticated: Boolean get() = authenticatedChannel != null

    init {
        try {
            socket.tcpNoDelay = true
            socket.connect(InetSocketAddress(ipAddress, port), 5000)
            authenticatedChannel = if (isOverAdb) {
                null
            } else {
                val token = ControlProtocol.decodeHex(pairingTokenHex ?: error("Pairing token required"))
                try {
                    ControlProtocol.authenticate(socket, token)
                } finally {
                    token.fill(0)
                }
            }
            socket.soTimeout = 0
            Logger.log("TCPConnection", if (isOverAdb) "Connected over authorized ADB" else "Authenticated control connection established")
        } catch (e: Exception) {
            try { socket.close() } catch (_: Exception) { }
            throw ConnectionFailedException("$ipAddress:$port", e.message ?: "connection failed")
        }
    }

    fun startReceiver() {
        if (::thread.isInitialized) return
        thread = Thread({ receiveLoop() }, "OpenCam-control-reader").apply { isDaemon = true; start() }
    }

    fun sendMessage(type: Int, payload: ByteArray) {
        require(payload.size <= maxPacketSize)
        if (isOverAdb) {
            synchronized(socket) {
                val wire = when (type) {
                    // Legacy Windows ADB descriptor framing: uint32 BE byte length,
                    // then exactly one serialized descriptor.
                    ControlProtocol.TYPE_DESCRIPTOR -> {
                        require(payload.size in 1..ControlProtocol.MAX_PAYLOAD)
                        ByteBuffer.allocate(4 + payload.size).order(ByteOrder.BIG_ENDIAN)
                            .putInt(payload.size).put(payload).array()
                    }
                    ControlProtocol.TYPE_ERROR_REPORT -> payload
                    else -> byteArrayOf(type.toByte()) + payload
                }
                socket.getOutputStream().write(wire)
                socket.getOutputStream().flush()
            }
        } else {
            authenticatedChannel?.send(type, payload)
                ?: throw IllegalStateException("Authenticated channel unavailable")
        }
    }

    override fun send(bytes: ByteArray) {
        if (!isOverAdb) throw IllegalStateException("Use framed messages on Wi-Fi")
        synchronized(socket) { socket.getOutputStream().write(bytes); socket.getOutputStream().flush() }
    }

    override fun send(bytes: ByteArray, size: Int) {
        require(size in 0..bytes.size)
        send(bytes.copyOf(size))
    }

    private fun receiveLoop() {
        try {
            if (isOverAdb) {
                val buffer = ByteArray(512)
                val input = socket.getInputStream()
                while (running.get()) {
                    val count = input.read(buffer)
                    if (count < 0) break
                    if (count > 0) listener.onLegacyBytes(this, buffer.copyOf(count), count)
                }
            } else {
                val channel = authenticatedChannel ?: return
                var activated = false
                while (running.get()) {
                    val frame = channel.receive(if (activated) -1 else 60000)
                    if (frame.type != ControlProtocol.TYPE_ACTIVATION || activated)
                        throw java.io.IOException("Unexpected server message")
                    activated = true
                    listener.onFrameReceived(this, frame.type, frame.payload)
                }
            }
        } catch (e: Exception) {
            if (running.get()) Logger.log("TCPConnection", "Control connection closed")
        } finally {
            if (running.getAndSet(false)) listener.onDisconnected(this)
            try { socket.close() } catch (_: Exception) { }
        }
    }

    override fun close() {
        if (!running.getAndSet(false)) return
        try { socket.close() } catch (_: Exception) { }
        if (::thread.isInitialized && Thread.currentThread() !== thread) {
            try { thread.join(2000) } catch (_: InterruptedException) { Thread.currentThread().interrupt() }
        }
    }
}
