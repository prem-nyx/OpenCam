package com.darusc.vcamdroid.networking

import java.io.DataInputStream
import java.io.DataOutputStream
import java.io.IOException
import java.net.Socket
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.charset.StandardCharsets
import java.security.MessageDigest
import java.security.SecureRandom
import javax.crypto.Mac
import javax.crypto.spec.SecretKeySpec

/** Versioned, bounded TCP framing and QR-secret authenticated session helpers. */
object ControlProtocol {
    const val VERSION = 1
    const val MAX_PAYLOAD = 64 * 1024
    const val TYPE_SERVER_HELLO = 1
    const val TYPE_CLIENT_AUTH = 2
    const val TYPE_SERVER_AUTH = 3
    const val TYPE_DESCRIPTOR = 4
    const val TYPE_ACTIVATION = 5
    const val TYPE_ERROR_REPORT = 6
    private val MAGIC = byteArrayOf('O'.code.toByte(), 'C'.code.toByte(), 'A'.code.toByte(), 'M'.code.toByte())
    private const val HEADER_SIZE = 10
    private const val AUTH_OVERHEAD = 40
    private const val HEX = "0123456789abcdef"

    data class Frame(val type: Int, val payload: ByteArray)

    fun decodeHex(text: String): ByteArray {
        require(text.length == 64 && text.all { it in "0123456789abcdefABCDEF" }) { "Invalid pairing token" }
        return ByteArray(32) { index -> text.substring(index * 2, index * 2 + 2).toInt(16).toByte() }
    }

    private fun mac(key: ByteArray, data: ByteArray): ByteArray {
        val hmac = Mac.getInstance("HmacSHA256")
        hmac.init(SecretKeySpec(key, "HmacSHA256"))
        return hmac.doFinal(data)
    }

    private fun proof(secret: ByteArray, domain: String, serverNonce: ByteArray, clientNonce: ByteArray) =
        mac(secret, domain.toByteArray(StandardCharsets.US_ASCII) + serverNonce + clientNonce)

    internal fun deriveMediaPassword(sessionKey: ByteArray): String {
        require(sessionKey.size == 32)
        val derived = mac(sessionKey, "OpenCam RTSP credential v1".toByteArray(StandardCharsets.US_ASCII))
        return try {
            buildString(derived.size * 2) {
                derived.forEach { value ->
                    val byte = value.toInt() and 0xff
                    append(HEX[byte ushr 4])
                    append(HEX[byte and 0x0f])
                }
            }
        } finally {
            derived.fill(0)
        }
    }

    class AuthenticatedChannel(private val socket: Socket, private val key: ByteArray) {
        private val input = DataInputStream(socket.getInputStream())
        private val output = DataOutputStream(socket.getOutputStream())
        // TCP is full duplex: the reader may block waiting for an activation while
        // the camera thread sends its descriptor. Never serialize those operations
        // on the same monitor, or neither side can make progress.
        private val sendLock = Any()
        private val receiveLock = Any()
        private var sendSequence = 0L
        private var receiveSequence = 0L

        fun mediaPassword(): String {
            return deriveMediaPassword(key)
        }

        fun send(type: Int, payload: ByteArray, timeoutMs: Int = 5000) = synchronized(sendLock) {
            require(payload.size + AUTH_OVERHEAD <= MAX_PAYLOAD) { "Message exceeds protocol limit" }
            val sequence = ByteBuffer.allocate(8).order(ByteOrder.BIG_ENDIAN).putLong(sendSequence).array()
            val tag = mac(key, byteArrayOf(type.toByte()) + sequence + payload)
            writeFrame(output, type, sequence + payload + tag, timeoutMs)
            sendSequence++
        }

        fun receive(timeoutMs: Int): Frame = synchronized(receiveLock) {
            val frame = readFrame(socket, input, timeoutMs)
            if (frame.payload.size < AUTH_OVERHEAD) throw IOException("Truncated authenticated frame")
            val sequenceBytes = frame.payload.copyOfRange(0, 8)
            val sequence = ByteBuffer.wrap(sequenceBytes).order(ByteOrder.BIG_ENDIAN).long
            if (sequence != receiveSequence) throw IOException("Invalid control sequence")
            val payloadEnd = frame.payload.size - 32
            val payload = frame.payload.copyOfRange(8, payloadEnd)
            val actualTag = frame.payload.copyOfRange(payloadEnd, frame.payload.size)
            val expectedTag = mac(key, byteArrayOf(frame.type.toByte()) + sequenceBytes + payload)
            if (!MessageDigest.isEqual(expectedTag, actualTag)) throw IOException("Invalid control authentication")
            receiveSequence++
            Frame(frame.type, payload)
        }
    }

    fun authenticate(socket: Socket, token: ByteArray): AuthenticatedChannel {
        require(token.size == 32) { "Invalid pairing token" }
        val input = DataInputStream(socket.getInputStream())
        val output = DataOutputStream(socket.getOutputStream())
        val hello = readFrame(socket, input, 5000)
        if (hello.type != TYPE_SERVER_HELLO || hello.payload.size != 32) throw IOException("Invalid server handshake")
        val serverNonce = hello.payload
        val clientNonce = ByteArray(32).also { SecureRandom().nextBytes(it) }
        val clientProof = proof(token, "OpenCam client proof v1", serverNonce, clientNonce)
        writeFrame(output, TYPE_CLIENT_AUTH, clientNonce + clientProof, 5000)
        val response = readFrame(socket, input, 5000)
        if (response.type != TYPE_SERVER_AUTH || response.payload.size != 32) throw IOException("Pairing rejected")
        val expected = proof(token, "OpenCam server proof v1", serverNonce, clientNonce)
        if (!MessageDigest.isEqual(expected, response.payload)) throw IOException("Server identity verification failed")
        val sessionKey = mac(token, "OpenCam session v1".toByteArray(StandardCharsets.US_ASCII) + serverNonce + clientNonce)
        return AuthenticatedChannel(socket, sessionKey)
    }

    private fun writeFrame(output: DataOutputStream, type: Int, payload: ByteArray, timeoutMs: Int) {
        require(payload.size <= MAX_PAYLOAD)
        val header = ByteBuffer.allocate(HEADER_SIZE).order(ByteOrder.BIG_ENDIAN)
            .put(MAGIC).put(VERSION.toByte()).put(type.toByte()).putInt(payload.size).array()
        output.write(header)
        output.write(payload)
        output.flush()
    }

    private fun readFrame(socket: Socket, input: DataInputStream, timeoutMs: Int): Frame {
        val deadline = if (timeoutMs < 0) Long.MAX_VALUE else System.nanoTime() + timeoutMs * 1_000_000L
        fun readExact(size: Int): ByteArray {
            val bytes = ByteArray(size)
            var offset = 0
            while (offset < size) {
                val remainingMs = if (timeoutMs < 0) 0 else
                    ((deadline - System.nanoTime()) / 1_000_000L).coerceAtLeast(1).toInt()
                socket.soTimeout = remainingMs
                val count = input.read(bytes, offset, size - offset)
                if (count < 0) throw IOException("Peer closed the connection")
                offset += count
            }
            return bytes
        }
        val header = readExact(HEADER_SIZE)
        val buffer = ByteBuffer.wrap(header).order(ByteOrder.BIG_ENDIAN)
        val magic = ByteArray(4).also(buffer::get)
        if (!magic.contentEquals(MAGIC) || (buffer.get().toInt() and 0xff) != VERSION)
            throw IOException("Invalid protocol header")
        val type = buffer.get().toInt() and 0xff
        val length = buffer.int
        if (length < 0 || length > MAX_PAYLOAD) throw IOException("Invalid message length")
        return Frame(type, readExact(length))
    }
}
