package com.darusc.vcamdroid.networking

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertThrows
import org.junit.Test
import java.io.DataInputStream
import java.io.DataOutputStream
import java.net.InetAddress
import java.net.ServerSocket
import java.net.Socket
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.charset.StandardCharsets
import javax.crypto.Mac
import javax.crypto.spec.SecretKeySpec
import kotlin.concurrent.thread

class ControlProtocolTest {
    private data class WireFrame(val type: Int, val payload: ByteArray)

    private fun hmac(key: ByteArray, bytes: ByteArray): ByteArray {
        val mac = Mac.getInstance("HmacSHA256")
        mac.init(SecretKeySpec(key, "HmacSHA256"))
        return mac.doFinal(bytes)
    }

    private fun writeFrame(output: DataOutputStream, type: Int, payload: ByteArray, fragmented: Boolean = false) {
        val bytes = ByteBuffer.allocate(10 + payload.size).order(ByteOrder.BIG_ENDIAN)
            .put(byteArrayOf(0x4f, 0x43, 0x41, 0x4d)).put(1).put(type.toByte()).putInt(payload.size).put(payload).array()
        if (fragmented) bytes.forEach { output.writeByte(it.toInt()) } else output.write(bytes)
        output.flush()
    }

    private fun readFrame(input: DataInputStream): WireFrame {
        val header = ByteArray(10)
        input.readFully(header)
        val buffer = ByteBuffer.wrap(header).order(ByteOrder.BIG_ENDIAN)
        assertArrayEquals(byteArrayOf(0x4f, 0x43, 0x41, 0x4d), ByteArray(4).also(buffer::get))
        assertEquals(1, buffer.get().toInt())
        val type = buffer.get().toInt() and 0xff
        val length = buffer.int
        require(length in 0..ControlProtocol.MAX_PAYLOAD)
        return WireFrame(type, ByteArray(length).also(input::readFully))
    }

    @Test fun qrTokenIsStrictlyValidated() {
        val token = "0123456789abcdef".repeat(4)
        assertEquals(32, ControlProtocol.decodeHex(token).size)
        assertThrows(IllegalArgumentException::class.java) { ControlProtocol.decodeHex("short") }
        assertThrows(IllegalArgumentException::class.java) { ControlProtocol.decodeHex("g".repeat(64)) }
    }

    @Test fun rtspCredentialDerivationMatchesLinuxHexEncoding() {
        val key = ByteArray(32)
        assertEquals("6971359bf756f8937f274a269eec51e357b04f38626c36eecc7227f07c559d7b",
            ControlProtocol.deriveMediaPassword(key))
    }

    @Test fun fragmentedMutualHandshakeAndAuthenticatedFrame() {
        val secret = ByteArray(32) { 0x41 }
        val serverNonce = ByteArray(32) { 0x12 }
        val server = ServerSocket(0, 1, InetAddress.getLoopbackAddress())
        var failure: Throwable? = null
        val serverThread = thread {
            try {
                server.accept().use { socket ->
                    val input = DataInputStream(socket.getInputStream())
                    val output = DataOutputStream(socket.getOutputStream())
                    writeFrame(output, ControlProtocol.TYPE_SERVER_HELLO, serverNonce, fragmented = true)
                    val auth = readFrame(input)
                    assertEquals(ControlProtocol.TYPE_CLIENT_AUTH, auth.type)
                    assertEquals(64, auth.payload.size)
                    val clientNonce = auth.payload.copyOfRange(0, 32)
                    val expectedClient = hmac(secret,
                        "OpenCam client proof v1".toByteArray(StandardCharsets.US_ASCII) + serverNonce + clientNonce)
                    assertArrayEquals(expectedClient, auth.payload.copyOfRange(32, 64))
                    val serverProof = hmac(secret,
                        "OpenCam server proof v1".toByteArray(StandardCharsets.US_ASCII) + serverNonce + clientNonce)
                    writeFrame(output, ControlProtocol.TYPE_SERVER_AUTH, serverProof)
                    val wire = readFrame(input)
                    assertEquals(ControlProtocol.TYPE_DESCRIPTOR, wire.type)
                    val sequence = wire.payload.copyOfRange(0, 8)
                    assertEquals(0L, ByteBuffer.wrap(sequence).order(ByteOrder.BIG_ENDIAN).long)
                    val data = wire.payload.copyOfRange(8, wire.payload.size - 32)
                    val sessionKey = hmac(secret,
                        "OpenCam session v1".toByteArray(StandardCharsets.US_ASCII) + serverNonce + clientNonce)
                    val expectedTag = hmac(sessionKey, byteArrayOf(ControlProtocol.TYPE_DESCRIPTOR.toByte()) + sequence + data)
                    assertArrayEquals(expectedTag, wire.payload.copyOfRange(wire.payload.size - 32, wire.payload.size))
                    assertArrayEquals(byteArrayOf(1, 2, 3), data)
                }
            } catch (t: Throwable) { failure = t }
        }

        Socket(InetAddress.getLoopbackAddress(), server.localPort).use { socket ->
            val channel = ControlProtocol.authenticate(socket, secret)
            channel.send(ControlProtocol.TYPE_DESCRIPTOR, byteArrayOf(1, 2, 3))
        }
        serverThread.join(3000)
        server.close()
        assertFalse(serverThread.isAlive)
        failure?.let { throw AssertionError("Test server failed", it) }
    }

    @Test fun wrongSecretCannotCompleteHandshake() {
        val secret = ByteArray(32) { 0x11 }
        val wrongSecret = ByteArray(32) { 0x22 }
        val nonce = ByteArray(32) { 0x33 }
        val server = ServerSocket(0, 1, InetAddress.getLoopbackAddress())
        val serverThread = thread {
            server.accept().use { socket ->
                val input = DataInputStream(socket.getInputStream())
                val output = DataOutputStream(socket.getOutputStream())
                writeFrame(output, ControlProtocol.TYPE_SERVER_HELLO, nonce)
                val auth = readFrame(input)
                val clientNonce = auth.payload.copyOfRange(0, 32)
                val valid = hmac(secret,
                    "OpenCam client proof v1".toByteArray(StandardCharsets.US_ASCII) + nonce + clientNonce)
                if (valid.contentEquals(auth.payload.copyOfRange(32, 64))) {
                    val proof = hmac(secret,
                        "OpenCam server proof v1".toByteArray(StandardCharsets.US_ASCII) + nonce + clientNonce)
                    writeFrame(output, ControlProtocol.TYPE_SERVER_AUTH, proof)
                }
            }
        }
        Socket(InetAddress.getLoopbackAddress(), server.localPort).use { socket ->
            assertThrows(java.io.IOException::class.java) { ControlProtocol.authenticate(socket, wrongSecret) }
        }
        serverThread.join(3000)
        server.close()
        assertFalse(serverThread.isAlive)
    }

    @Test fun blockedReceiveDoesNotBlockFullDuplexDescriptorSend() {
        val key = ByteArray(32) { 0x5a }
        val server = ServerSocket(0, 1, InetAddress.getLoopbackAddress())
        var serverFailure: Throwable? = null
        val serverThread = thread {
            try {
                server.accept().use { socket ->
                    val input = DataInputStream(socket.getInputStream())
                    val output = DataOutputStream(socket.getOutputStream())
                    val descriptor = readFrame(input)
                    assertEquals(ControlProtocol.TYPE_DESCRIPTOR, descriptor.type)
                    val sequence = ByteBuffer.allocate(8).order(ByteOrder.BIG_ENDIAN).putLong(0).array()
                    val payload = byteArrayOf(7)
                    val tag = hmac(key, byteArrayOf(ControlProtocol.TYPE_ACTIVATION.toByte()) + sequence + payload)
                    writeFrame(output, ControlProtocol.TYPE_ACTIVATION, sequence + payload + tag)
                }
            } catch (t: Throwable) { serverFailure = t }
        }

        Socket(InetAddress.getLoopbackAddress(), server.localPort).use { socket ->
            val channel = ControlProtocol.AuthenticatedChannel(socket, key)
            var activation: ControlProtocol.Frame? = null
            val receiveThread = thread { activation = channel.receive(3000) }
            Thread.sleep(100)
            val started = System.nanoTime()
            channel.send(ControlProtocol.TYPE_DESCRIPTOR, byteArrayOf(1, 2, 3))
            val sendMillis = (System.nanoTime() - started) / 1_000_000
            receiveThread.join(2000)
            assertFalse(receiveThread.isAlive)
            assertEquals(ControlProtocol.TYPE_ACTIVATION, activation?.type)
            assertArrayEquals(byteArrayOf(7), activation?.payload)
            assert(sendMillis < 1000) { "send waited behind receive for ${sendMillis}ms" }
        }
        serverThread.join(3000)
        server.close()
        assertFalse(serverThread.isAlive)
        serverFailure?.let { throw AssertionError("Test server failed", it) }
    }
}
