package com.darusc.vcamdroid.networking

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertThrows
import org.junit.Test
import java.io.DataInputStream
import java.io.DataOutputStream
import java.io.IOException
import java.net.InetAddress
import java.net.ServerSocket
import java.net.Socket
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.charset.StandardCharsets
import javax.crypto.Cipher
import javax.crypto.Mac
import javax.crypto.spec.GCMParameterSpec
import javax.crypto.spec.SecretKeySpec
import kotlin.concurrent.thread

class ControlProtocolTest {

    private data class WireFrame(
        val type: Int,
        val payload: ByteArray
    )

    private val magic = byteArrayOf(
        0x4f,
        0x43,
        0x41,
        0x4d
    )

    private fun hmac(
        key: ByteArray,
        bytes: ByteArray
    ): ByteArray {
        val mac = Mac.getInstance("HmacSHA256")
        mac.init(
            SecretKeySpec(
                key,
                "HmacSHA256"
            )
        )
        return mac.doFinal(bytes)
    }

    private fun hkdfExtract(
        salt: ByteArray,
        inputKeyMaterial: ByteArray
    ): ByteArray {
        return hmac(
            salt,
            inputKeyMaterial
        )
    }

    private fun hkdfExpand(
        prk: ByteArray,
        info: ByteArray,
        outputLength: Int
    ): ByteArray {
        val output = ByteArray(outputLength)

        var previous = ByteArray(0)
        var offset = 0
        var counter = 1

        while (offset < outputLength) {
            previous = hmac(
                prk,
                previous +
                    info +
                    byteArrayOf(counter.toByte())
            )

            val copyLength = minOf(
                previous.size,
                outputLength - offset
            )

            System.arraycopy(
                previous,
                0,
                output,
                offset,
                copyLength
            )

            offset += copyLength
            counter++
        }

        previous.fill(0)

        return output
    }

    private fun deriveSessionKey(
        secret: ByteArray,
        serverNonce: ByteArray,
        clientNonce: ByteArray
    ): ByteArray {
        val salt = serverNonce + clientNonce

        val prk = hkdfExtract(
            salt,
            secret
        )

        return try {
            hkdfExpand(
                prk,
                "OpenCam control v2 session"
                    .toByteArray(StandardCharsets.US_ASCII),
                32
            )
        } finally {
            prk.fill(0)
            salt.fill(0)
        }
    }

    private fun deriveDirectionalKey(
        sessionKey: ByteArray,
        info: String
    ): ByteArray {
        return hkdfExpand(
            sessionKey,
            info.toByteArray(StandardCharsets.US_ASCII),
            32
        )
    }

    private fun proof(
        secret: ByteArray,
        domain: String,
        serverNonce: ByteArray,
        clientNonce: ByteArray
    ): ByteArray {
        return hmac(
            secret,
            domain.toByteArray(StandardCharsets.US_ASCII) +
                serverNonce +
                clientNonce
        )
    }

    private fun writeFrame(
        output: DataOutputStream,
        type: Int,
        payload: ByteArray,
        fragmented: Boolean = false
    ) {
        val bytes =
            ByteBuffer
                .allocate(10 + payload.size)
                .order(ByteOrder.BIG_ENDIAN)
                .put(magic)
                .put(2)
                .put(type.toByte())
                .putInt(payload.size)
                .put(payload)
                .array()

        if (fragmented) {
            bytes.forEach {
                output.writeByte(it.toInt())
            }
        } else {
            output.write(bytes)
        }

        output.flush()
    }

    private fun readFrame(
        input: DataInputStream
    ): WireFrame {
        val header = ByteArray(10)

        input.readFully(header)

        val buffer =
            ByteBuffer
                .wrap(header)
                .order(ByteOrder.BIG_ENDIAN)

        assertArrayEquals(
            magic,
            ByteArray(4).also {
                buffer.get(it)
            }
        )

        assertEquals(
            2,
            buffer.get().toInt()
        )

        val type =
            buffer.get().toInt() and 0xff

        val length =
            buffer.int

        require(
            length in 0..ControlProtocol.MAX_PAYLOAD
        )

        return WireFrame(
            type,
            ByteArray(length).also {
                input.readFully(it)
            }
        )
    }

    private fun encryptServerFrame(
        sessionKey: ByteArray,
        type: Int,
        sequence: Long,
        plaintext: ByteArray
    ): ByteArray {

        val sendKey =
            deriveDirectionalKey(
                sessionKey,
                "OpenCam control v2 server-to-client"
            )

        val sequenceBytes =
            ByteBuffer
                .allocate(8)
                .order(ByteOrder.BIG_ENDIAN)
                .putLong(sequence)
                .array()

        val wirePayloadSize =
            8 +
                plaintext.size +
                16

        val header =
            ByteBuffer
                .allocate(10)
                .order(ByteOrder.BIG_ENDIAN)
                .put(magic)
                .put(2)
                .put(type.toByte())
                .putInt(wirePayloadSize)
                .array()

        val nonce =
            byteArrayOf(
                'S'.code.toByte(),
                'R'.code.toByte(),
                'V'.code.toByte(),
                'R'.code.toByte()
            ) + sequenceBytes

        val cipher =
            Cipher.getInstance(
                "AES/GCM/NoPadding"
            )

        cipher.init(
            Cipher.ENCRYPT_MODE,
            SecretKeySpec(
                sendKey,
                "AES"
            ),
            GCMParameterSpec(
                128,
                nonce
            )
        )

        cipher.updateAAD(header)

        val encrypted =
            cipher.doFinal(plaintext)

        return sequenceBytes + encrypted
    }

    @Test
    fun qrTokenIsStrictlyValidated() {
        val token =
            "0123456789abcdef".repeat(4)

        assertEquals(
            32,
            ControlProtocol.decodeHex(token).size
        )

        assertThrows(
            IllegalArgumentException::class.java
        ) {
            ControlProtocol.decodeHex("short")
        }

        assertThrows(
            IllegalArgumentException::class.java
        ) {
            ControlProtocol.decodeHex("g".repeat(64))
        }
    }

    @Test
    fun rtspCredentialDerivationMatchesLinuxHexEncoding() {
        val key = ByteArray(32)

        assertEquals(
            "3d71315941fed222d06ace2d99f2f5210fdbe4df5c3be2dc673e13c414bac9b5",
            ControlProtocol.deriveMediaPassword(key)
        )
    }

    @Test
    fun fragmentedMutualHandshakeAndAuthenticatedFrame() {
        val secret =
            ByteArray(32) {
                0x41
            }

        val serverNonce =
            ByteArray(32) {
                0x12
            }

        val server =
            ServerSocket(
                0,
                1,
                InetAddress.getLoopbackAddress()
            )

        var failure: Throwable? = null

        val serverThread =
            thread {
                try {
                    server.accept().use { socket ->

                        val input =
                            DataInputStream(
                                socket.getInputStream()
                            )

                        val output =
                            DataOutputStream(
                                socket.getOutputStream()
                            )

                        /*
                         * SERVER_HELLO
                         */
                        writeFrame(
                            output,
                            ControlProtocol.TYPE_SERVER_HELLO,
                            serverNonce,
                            fragmented = true
                        )

                        /*
                         * CLIENT_AUTH
                         */
                        val auth =
                            readFrame(input)

                        assertEquals(
                            ControlProtocol.TYPE_CLIENT_AUTH,
                            auth.type
                        )

                        assertEquals(
                            64,
                            auth.payload.size
                        )

                        val clientNonce =
                            auth.payload.copyOfRange(
                                0,
                                32
                            )

                        val expectedClient =
                            proof(
                                secret,
                                "OpenCam client proof v2",
                                serverNonce,
                                clientNonce
                            )

                        assertArrayEquals(
                            expectedClient,
                            auth.payload.copyOfRange(
                                32,
                                64
                            )
                        )

                        /*
                         * SERVER_AUTH
                         */
                        val serverProof =
                            proof(
                                secret,
                                "OpenCam server proof v2",
                                serverNonce,
                                clientNonce
                            )

                        writeFrame(
                            output,
                            ControlProtocol.TYPE_SERVER_AUTH,
                            serverProof
                        )

                        /*
                         * Derive exactly the same session key
                         * as Android.
                         */
                        val sessionKey =
                            deriveSessionKey(
                                secret,
                                serverNonce,
                                clientNonce
                            )

                        /*
                         * Switch to the real v2 encrypted
                         * server-side channel.
                         */
                        val channel =
                            ControlProtocol.AuthenticatedChannel(
                                socket,
                                sessionKey,
                                true
                            )

                        val frame =
                            channel.receive(3000)

                        assertEquals(
                            ControlProtocol.TYPE_DESCRIPTOR,
                            frame.type
                        )

                        assertArrayEquals(
                            byteArrayOf(1, 2, 3),
                            frame.payload
                        )
                    }
                } catch (t: Throwable) {
                    failure = t
                }
            }

        Socket(
            InetAddress.getLoopbackAddress(),
            server.localPort
        ).use { socket ->

            val channel =
                ControlProtocol.authenticate(
                    socket,
                    secret
                )

            channel.send(
                ControlProtocol.TYPE_DESCRIPTOR,
                byteArrayOf(1, 2, 3)
            )
        }

        serverThread.join(3000)

        server.close()

        assertFalse(
            serverThread.isAlive
        )

        failure?.let {
            throw AssertionError(
                "Test server failed",
                it
            )
        }
    }

    @Test
    fun wrongSecretCannotCompleteHandshake() {
        val secret =
            ByteArray(32) {
                0x11
            }

        val wrongSecret =
            ByteArray(32) {
                0x22
            }

        val nonce =
            ByteArray(32) {
                0x33
            }

        val server =
            ServerSocket(
                0,
                1,
                InetAddress.getLoopbackAddress()
            )

        val serverThread =
            thread {
                server.accept().use { socket ->

                    val input =
                        DataInputStream(
                            socket.getInputStream()
                        )

                    val output =
                        DataOutputStream(
                            socket.getOutputStream()
                        )

                    writeFrame(
                        output,
                        ControlProtocol.TYPE_SERVER_HELLO,
                        nonce
                    )

                    val auth =
                        readFrame(input)

                    val clientNonce =
                        auth.payload.copyOfRange(
                            0,
                            32
                        )

                    val valid =
                        proof(
                            secret,
                            "OpenCam client proof v2",
                            nonce,
                            clientNonce
                        )

                    if (
                        valid.contentEquals(
                            auth.payload.copyOfRange(
                                32,
                                64
                            )
                        )
                    ) {
                        val serverProof =
                            proof(
                                secret,
                                "OpenCam server proof v2",
                                nonce,
                                clientNonce
                            )

                        writeFrame(
                            output,
                            ControlProtocol.TYPE_SERVER_AUTH,
                            serverProof
                        )
                    }
                }
            }

        Socket(
            InetAddress.getLoopbackAddress(),
            server.localPort
        ).use { socket ->

            assertThrows(
                IOException::class.java
            ) {
                ControlProtocol.authenticate(
                    socket,
                    wrongSecret
                )
            }
        }

        serverThread.join(3000)

        server.close()

        assertFalse(
            serverThread.isAlive
        )
    }

    @Test
    fun blockedReceiveDoesNotBlockFullDuplexDescriptorSend() {
        val key =
            ByteArray(32) {
                0x5a
            }

        val server =
            ServerSocket(
                0,
                1,
                InetAddress.getLoopbackAddress()
            )

        var serverFailure: Throwable? = null

        val serverThread =
            thread {
                try {
                    server.accept().use { socket ->

                        val channel =
                            ControlProtocol.AuthenticatedChannel(
                                socket,
                                key,
                                true
                            )

                        /*
                         * Receive the descriptor from Android.
                         */
                        val descriptor =
                            channel.receive(3000)

                        assertEquals(
                            ControlProtocol.TYPE_DESCRIPTOR,
                            descriptor.type
                        )

                        assertArrayEquals(
                            byteArrayOf(1, 2, 3),
                            descriptor.payload
                        )

                        /*
                         * Send activation back using the
                         * real v2 encrypted channel.
                         */
                        channel.send(
                            ControlProtocol.TYPE_ACTIVATION,
                            byteArrayOf(7)
                        )
                    }
                } catch (t: Throwable) {
                    serverFailure = t
                }
            }

        Socket(
            InetAddress.getLoopbackAddress(),
            server.localPort
        ).use { socket ->

            val channel =
                ControlProtocol.AuthenticatedChannel(
                    socket,
                    key,
                    false
                )

            var activation:
                ControlProtocol.Frame? = null

            val receiveThread =
                thread {
                    activation =
                        channel.receive(3000)
                }

            Thread.sleep(100)

            val started =
                System.nanoTime()

            channel.send(
                ControlProtocol.TYPE_DESCRIPTOR,
                byteArrayOf(1, 2, 3)
            )

            val sendMillis =
                (
                    System.nanoTime() -
                        started
                ) / 1_000_000

            receiveThread.join(2000)

            assertFalse(
                receiveThread.isAlive
            )

            assertEquals(
                ControlProtocol.TYPE_ACTIVATION,
                activation?.type
            )

            assertArrayEquals(
                byteArrayOf(7),
                activation?.payload
            )

            assert(
                sendMillis < 1000
            ) {
                "send waited behind receive for ${sendMillis}ms"
            }
        }

        serverThread.join(3000)

        server.close()

        assertFalse(
            serverThread.isAlive
        )

        serverFailure?.let {
            throw AssertionError(
                "Test server failed",
                it
            )
        }
    }

@Test
fun tamperedEncryptedFrameIsRejected() {
    val key = ByteArray(32) { 0x5a }

    val server = ServerSocket(
        0,
        1,
        InetAddress.getLoopbackAddress()
    )

    var serverFailure: Throwable? = null

    val serverThread = thread {
        try {
            server.accept().use { socket ->

                val channel =
                    ControlProtocol.AuthenticatedChannel(
                        socket,
                        key,
                        true
                    )

                assertThrows(IOException::class.java) {
                    channel.receive(3000)
                }
            }
        } catch (t: Throwable) {
            serverFailure = t
        }
    }

    Socket(
        InetAddress.getLoopbackAddress(),
        server.localPort
    ).use { socket ->

        /*
         * We need to put one valid encrypted frame on
         * the wire and modify its ciphertext before the
         * server receives it.
         */
        val output =
            DataOutputStream(
                socket.getOutputStream()
            )

        val plaintext = byteArrayOf(1, 2, 3)

        /*
         * Client-side directional key.
         */
        val sendKey =
            deriveDirectionalKey(
                key,
                "OpenCam control v2 client-to-server"
            )

        val sequence =
            ByteBuffer
                .allocate(8)
                .order(ByteOrder.BIG_ENDIAN)
                .putLong(0)
                .array()

        val wirePayloadSize =
            8 + plaintext.size + 16

        val header =
            ByteBuffer
                .allocate(10)
                .order(ByteOrder.BIG_ENDIAN)
                .put(magic)
                .put(2)
                .put(
                    ControlProtocol.TYPE_DESCRIPTOR
                        .toByte()
                )
                .putInt(wirePayloadSize)
                .array()

        val nonce =
            byteArrayOf(
                'C'.code.toByte(),
                'L'.code.toByte(),
                'N'.code.toByte(),
                'T'.code.toByte()
            ) + sequence

        val cipher =
            Cipher.getInstance(
                "AES/GCM/NoPadding"
            )

        cipher.init(
            Cipher.ENCRYPT_MODE,
            SecretKeySpec(
                sendKey,
                "AES"
            ),
            GCMParameterSpec(
                128,
                nonce
            )
        )

        cipher.updateAAD(header)

        val encrypted =
            cipher.doFinal(plaintext)

        /*
         * 🔥 THE ATTACK:
         * Flip exactly one bit in the ciphertext.
         */
        encrypted[0] =
            (encrypted[0].toInt() xor 0x01).toByte()

        output.write(header)
        output.write(sequence)
        output.write(encrypted)
        output.flush()
    }

    serverThread.join(3000)

    server.close()

    assertFalse(
        serverThread.isAlive
    )

    serverFailure?.let {
        throw AssertionError(
            "Tamper test server failed",
            it
        )
    }
}

}