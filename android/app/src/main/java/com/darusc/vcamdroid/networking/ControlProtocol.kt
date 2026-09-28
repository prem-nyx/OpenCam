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
import javax.crypto.Cipher
import javax.crypto.Mac
import javax.crypto.spec.GCMParameterSpec
import javax.crypto.spec.SecretKeySpec

/**
 * OpenCam Control Protocol v2.
 *
 * Handshake:
 *   SERVER_HELLO
 *   CLIENT_AUTH
 *   SERVER_AUTH
 *
 * After authentication:
 *   AES-256-GCM encrypted frames
 *
 * Each direction has an independent AES key.
 */
object ControlProtocol {

    const val VERSION = 2
    const val MAX_PAYLOAD = 64 * 1024

    const val TYPE_SERVER_HELLO = 1
    const val TYPE_CLIENT_AUTH = 2
    const val TYPE_SERVER_AUTH = 3
    const val TYPE_DESCRIPTOR = 4
    const val TYPE_ACTIVATION = 5
    const val TYPE_ERROR_REPORT = 6

    private val MAGIC = byteArrayOf(
        'O'.code.toByte(),
        'C'.code.toByte(),
        'A'.code.toByte(),
        'M'.code.toByte()
    )

    private const val HEADER_SIZE = 10
    private const val SEQUENCE_SIZE = 8
    private const val KEY_SIZE = 32
    private const val GCM_NONCE_SIZE = 12
    private const val GCM_TAG_SIZE = 16

    /*
     * Encrypted payload:
     *
     *   sequence (8 bytes)
     *   ciphertext
     *   GCM tag (16 bytes)
     */
    private const val ENCRYPTED_OVERHEAD =
        SEQUENCE_SIZE + GCM_TAG_SIZE

    private const val HEX = "0123456789abcdef"

    private val SERVER_NONCE_PREFIX = byteArrayOf(
        'S'.code.toByte(),
        'R'.code.toByte(),
        'V'.code.toByte(),
        'R'.code.toByte()
    )

    private val CLIENT_NONCE_PREFIX = byteArrayOf(
        'C'.code.toByte(),
        'L'.code.toByte(),
        'N'.code.toByte(),
        'T'.code.toByte()
    )

    private const val SESSION_KDF_INFO =
        "OpenCam control v2 session"

    private const val SERVER_TO_CLIENT_KDF_INFO =
        "OpenCam control v2 server-to-client"

    private const val CLIENT_TO_SERVER_KDF_INFO =
        "OpenCam control v2 client-to-server"

    private const val MEDIA_CREDENTIAL_DOMAIN =
        "OpenCam RTSP credential v2"

    data class Frame(
        val type: Int,
        val payload: ByteArray
    )

    /*
     * ------------------------------------------------------------------------
     * Basic cryptographic helpers
     * ------------------------------------------------------------------------
     */

    private fun mac(
        key: ByteArray,
        data: ByteArray
    ): ByteArray {
        val hmac = Mac.getInstance("HmacSHA256")

        hmac.init(
            SecretKeySpec(
                key,
                "HmacSHA256"
            )
        )

        return hmac.doFinal(data)
    }

    /*
     * HKDF-Expand using HMAC-SHA256.
     *
     * HKDF-Extract is:
     *
     *   PRK = HMAC(salt, inputKeyMaterial)
     *
     * HKDF-Expand is then:
     *
     *   T(1) = HMAC(PRK, info || 0x01)
     *   T(2) = HMAC(PRK, T(1) || info || 0x02)
     *   ...
     */
    private fun hkdfExtract(
        salt: ByteArray,
        inputKeyMaterial: ByteArray
    ): ByteArray {
        return mac(
            salt,
            inputKeyMaterial
        )
    }

    private fun hkdfExpand(
        prk: ByteArray,
        info: ByteArray,
        outputLength: Int
    ): ByteArray {
        require(outputLength >= 0)
        require(outputLength <= 255 * 32)

        val output = ByteArray(outputLength)

        var previous = ByteArray(0)
        var offset = 0
        var counter = 1

        while (offset < outputLength) {

            previous = mac(
                prk,
                previous +
                    info +
                    byteArrayOf(counter.toByte())
            )

            val copyLength =
                minOf(
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

        require(secret.size == KEY_SIZE)
        require(serverNonce.size == 32)
        require(clientNonce.size == 32)

        /*
         * Linux uses:
         *
         *   salt = serverNonce || clientNonce
         *   PRK  = HMAC(salt, secret)
         */
        val salt =
            serverNonce + clientNonce

        val prk =
            hkdfExtract(
                salt,
                secret
            )

        return try {
            hkdfExpand(
                prk,
                SESSION_KDF_INFO.toByteArray(
                    StandardCharsets.US_ASCII
                ),
                KEY_SIZE
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
        /*
         * The session key acts as the HKDF PRK here.
         *
         * Direction-specific info ensures that the two directions
         * never share the same AES key.
         */
        return hkdfExpand(
            sessionKey,
            info.toByteArray(
                StandardCharsets.US_ASCII
            ),
            KEY_SIZE
        )
    }

    /*
     * ------------------------------------------------------------------------
     * Pairing / authentication
     * ------------------------------------------------------------------------
     */

    private fun proof(
        secret: ByteArray,
        domain: String,
        serverNonce: ByteArray,
        clientNonce: ByteArray
    ): ByteArray {
        return mac(
            secret,
            domain.toByteArray(
                StandardCharsets.US_ASCII
            ) +
                serverNonce +
                clientNonce
        )
    }

    /*
     * ------------------------------------------------------------------------
     * RTSP credential derivation
     * ------------------------------------------------------------------------
     */

    internal fun deriveMediaPassword(
        sessionKey: ByteArray
    ): String {

        require(sessionKey.size == KEY_SIZE)

        val derived =
            mac(
                sessionKey,
                MEDIA_CREDENTIAL_DOMAIN.toByteArray(
                    StandardCharsets.US_ASCII
                )
            )

        return try {
            buildString(
                derived.size * 2
            ) {
                derived.forEach { value ->

                    val byte =
                        value.toInt() and 0xff

                    append(
                        HEX[byte ushr 4]
                    )

                    append(
                        HEX[byte and 0x0f]
                    )
                }
            }
        } finally {
            derived.fill(0)
        }
    }

    /*
     * ------------------------------------------------------------------------
     * Wire-format helpers
     * ------------------------------------------------------------------------
     */

    private fun makeSequence(
        sequence: Long
    ): ByteArray {
        return ByteBuffer
            .allocate(SEQUENCE_SIZE)
            .order(ByteOrder.BIG_ENDIAN)
            .putLong(sequence)
            .array()
    }

    private fun makeHeader(
        type: Int,
        payloadSize: Int
    ): ByteArray {
        return ByteBuffer
            .allocate(HEADER_SIZE)
            .order(ByteOrder.BIG_ENDIAN)
            .put(MAGIC)
            .put(VERSION.toByte())
            .put(type.toByte())
            .putInt(payloadSize)
            .array()
    }

    private fun makeNonce(
        prefix: ByteArray,
        sequence: ByteArray
    ): ByteArray {
        require(prefix.size == 4)
        require(sequence.size == 8)

        return prefix + sequence
    }

    /*
     * ------------------------------------------------------------------------
     * AES-256-GCM
     * ------------------------------------------------------------------------
     */

    private fun encrypt(
        key: ByteArray,
        nonce: ByteArray,
        aad: ByteArray,
        plaintext: ByteArray
    ): ByteArray {

        return try {

            val cipher =
                Cipher.getInstance(
                    "AES/GCM/NoPadding"
                )

            cipher.init(
                Cipher.ENCRYPT_MODE,
                SecretKeySpec(
                    key,
                    "AES"
                ),
                GCMParameterSpec(
                    GCM_TAG_SIZE * 8,
                    nonce
                )
            )

            /*
             * Authenticate the outer frame header without
             * encrypting it.
             */
            cipher.updateAAD(aad)

            /*
             * Java's GCM implementation returns:
             *
             *   ciphertext || authentication tag
             */
            cipher.doFinal(plaintext)

        } catch (error: Exception) {

            throw IOException(
                "Failed to encrypt control frame",
                error
            )
        }
    }

    private fun decrypt(
        key: ByteArray,
        nonce: ByteArray,
        aad: ByteArray,
        ciphertextAndTag: ByteArray
    ): ByteArray {

        return try {

            val cipher =
                Cipher.getInstance(
                    "AES/GCM/NoPadding"
                )

            cipher.init(
                Cipher.DECRYPT_MODE,
                SecretKeySpec(
                    key,
                    "AES"
                ),
                GCMParameterSpec(
                    GCM_TAG_SIZE * 8,
                    nonce
                )
            )

            cipher.updateAAD(aad)

            /*
             * doFinal() verifies the GCM authentication tag.
             *
             * If anything was modified, this throws.
             */
            cipher.doFinal(
                ciphertextAndTag
            )

        } catch (error: Exception) {

            throw IOException(
                "Invalid encrypted control frame",
                error
            )
        }
    }

    /*
     * ------------------------------------------------------------------------
     * Authenticated + encrypted channel
     * ------------------------------------------------------------------------
     */

    class AuthenticatedChannel(
        private val socket: Socket,
        sessionKey: ByteArray,
        private val serverSide: Boolean
    ) {

        private val input =
            DataInputStream(
                socket.getInputStream()
            )

        private val output =
            DataOutputStream(
                socket.getOutputStream()
            )

        /*
         * Keep the session key because it is also used to derive
         * the RTSP credential.
         */
        private val sessionKey =
            sessionKey.copyOf()

        private val sendKey: ByteArray
        private val receiveKey: ByteArray

        /*
         * TCP is full duplex.
         *
         * A blocked receiver must never prevent the other direction
         * from sending.
         */
        private val sendLock = Any()
        private val receiveLock = Any()

        private var sendSequence = 0L
        private var receiveSequence = 0L

        init {

            require(
                sessionKey.size == KEY_SIZE
            )

            sendKey =
                deriveDirectionalKey(
                    this.sessionKey,
                    if (serverSide) {
                        SERVER_TO_CLIENT_KDF_INFO
                    } else {
                        CLIENT_TO_SERVER_KDF_INFO
                    }
                )

            receiveKey =
                deriveDirectionalKey(
                    this.sessionKey,
                    if (serverSide) {
                        CLIENT_TO_SERVER_KDF_INFO
                    } else {
                        SERVER_TO_CLIENT_KDF_INFO
                    }
                )
        }

        fun mediaPassword(): String {
            return deriveMediaPassword(
                sessionKey
            )
        }

        fun send(
            type: Int,
            payload: ByteArray,
            timeoutMs: Int = 5000
        ) = synchronized(sendLock) {

            if (sendSequence == Long.MAX_VALUE) {
                throw IOException(
                    "Control sequence exhausted"
                )
            }

            require(
                payload.size +
                    ENCRYPTED_OVERHEAD <=
                    MAX_PAYLOAD
            ) {
                "Message exceeds protocol limit"
            }

            /*
             * Sequence number is not encrypted.
             *
             * It is needed to construct the nonce and is
             * independently checked on reception.
             */
            val sequence =
                makeSequence(
                    sendSequence
                )

            /*
             * Wire payload:
             *
             *   sequence + ciphertext + GCM tag
             */
            val wirePayloadSize =
                SEQUENCE_SIZE +
                    payload.size +
                    GCM_TAG_SIZE

            /*
             * The header is authenticated as AES-GCM AAD.
             */
            val header =
                makeHeader(
                    type,
                    wirePayloadSize
                )

            val nonce =
                makeNonce(
                    if (serverSide) {
                        SERVER_NONCE_PREFIX
                    } else {
                        CLIENT_NONCE_PREFIX
                    },
                    sequence
                )

            val encrypted =
                encrypt(
                    sendKey,
                    nonce,
                    header,
                    payload
                )

            writeFrame(
                output,
                type,
                sequence + encrypted,
                timeoutMs
            )

            sendSequence++
        }

        fun receive(
            timeoutMs: Int
        ): Frame = synchronized(receiveLock) {

            if (receiveSequence == Long.MAX_VALUE) {
                throw IOException(
                    "Control sequence exhausted"
                )
            }

            val frame =
                readFrame(
                    socket,
                    input,
                    timeoutMs
                )

            if (
                frame.payload.size <
                    ENCRYPTED_OVERHEAD
            ) {
                throw IOException(
                    "Truncated encrypted frame"
                )
            }

            val sequenceBytes =
                frame.payload.copyOfRange(
                    0,
                    SEQUENCE_SIZE
                )

            val sequence =
                ByteBuffer
                    .wrap(sequenceBytes)
                    .order(ByteOrder.BIG_ENDIAN)
                    .long

            /*
             * Strict ordering prevents replay and reordering.
             */
            if (
                sequence !=
                    receiveSequence
            ) {
                throw IOException(
                    "Invalid control sequence"
                )
            }

            val ciphertextAndTag =
                frame.payload.copyOfRange(
                    SEQUENCE_SIZE,
                    frame.payload.size
                )

            val header =
                makeHeader(
                    frame.type,
                    frame.payload.size
                )

            val nonce =
                makeNonce(
                    if (serverSide) {
                        CLIENT_NONCE_PREFIX
                    } else {
                        SERVER_NONCE_PREFIX
                    },
                    sequenceBytes
                )

            val plaintext =
                decrypt(
                    receiveKey,
                    nonce,
                    header,
                    ciphertextAndTag
                )

            receiveSequence++

            Frame(
                frame.type,
                plaintext
            )
        }
    }

    /*
     * ------------------------------------------------------------------------
     * Authentication handshake
     * ------------------------------------------------------------------------
     */

    fun authenticate(
        socket: Socket,
        token: ByteArray
    ): AuthenticatedChannel {

        require(
            token.size == KEY_SIZE
        ) {
            "Invalid pairing token"
        }

        val input =
            DataInputStream(
                socket.getInputStream()
            )

        val output =
            DataOutputStream(
                socket.getOutputStream()
            )

        /*
         * 1. Receive server nonce.
         */
        val hello =
            readFrame(
                socket,
                input,
                5000
            )

        if (
            hello.type !=
                TYPE_SERVER_HELLO ||
            hello.payload.size != 32
        ) {
            throw IOException(
                "Invalid server handshake"
            )
        }

        val serverNonce =
            hello.payload

        /*
         * 2. Generate fresh client nonce.
         */
        val clientNonce =
            ByteArray(32).also {
                SecureRandom().nextBytes(it)
            }

        /*
         * 3. Prove possession of the QR secret.
         */
        val clientProof =
            proof(
                token,
                "OpenCam client proof v2",
                serverNonce,
                clientNonce
            )

        writeFrame(
            output,
            TYPE_CLIENT_AUTH,
            clientNonce + clientProof,
            5000
        )

        /*
         * 4. Verify server proof.
         */
        val response =
            readFrame(
                socket,
                input,
                5000
            )

        if (
            response.type !=
                TYPE_SERVER_AUTH ||
            response.payload.size != 32
        ) {
            throw IOException(
                "Pairing rejected"
            )
        }

        val expected =
            proof(
                token,
                "OpenCam server proof v2",
                serverNonce,
                clientNonce
            )

        if (
            !MessageDigest.isEqual(
                expected,
                response.payload
            )
        ) {
            throw IOException(
                "Server identity verification failed"
            )
        }

        /*
         * 5. Both sides independently derive
         *    the same session key.
         */
        val sessionKey =
            deriveSessionKey(
                token,
                serverNonce,
                clientNonce
            )

        /*
         * Android is the client, therefore serverSide=false.
         */
        return AuthenticatedChannel(
            socket,
            sessionKey,
            false
        )
    }

    /*
     * ------------------------------------------------------------------------
     * Plain handshake framing
     * ------------------------------------------------------------------------
     *
     * Only the handshake uses this directly.
     * Post-authentication frames go through AES-GCM.
     */

    private fun writeFrame(
        output: DataOutputStream,
        type: Int,
        payload: ByteArray,
        @Suppress("UNUSED_PARAMETER")
        timeoutMs: Int
    ) {

        require(
            payload.size <= MAX_PAYLOAD
        )

        val header =
            makeHeader(
                type,
                payload.size
            )

        output.write(header)
        output.write(payload)
        output.flush()
    }

    private fun readFrame(
        socket: Socket,
        input: DataInputStream,
        timeoutMs: Int
    ): Frame {

        val deadline =
            if (timeoutMs < 0) {
                Long.MAX_VALUE
            } else {
                System.nanoTime() +
                    timeoutMs * 1_000_000L
            }

        fun readExact(
            size: Int
        ): ByteArray {

            val bytes =
                ByteArray(size)

            var offset = 0

            while (offset < size) {

                val remainingMs =
                    if (timeoutMs < 0) {
                        0
                    } else {
                        (
                            (
                                deadline -
                                    System.nanoTime()
                            ) / 1_000_000L
                        )
                            .coerceAtLeast(1)
                            .toInt()
                    }

                socket.soTimeout =
                    remainingMs

                val count =
                    input.read(
                        bytes,
                        offset,
                        size - offset
                    )

                if (count < 0) {
                    throw IOException(
                        "Peer closed the connection"
                    )
                }

                offset += count
            }

            return bytes
        }

        val header =
            readExact(
                HEADER_SIZE
            )

        val buffer =
            ByteBuffer
                .wrap(header)
                .order(ByteOrder.BIG_ENDIAN)

        val magic =
            ByteArray(4).also {
                buffer.get(it)
            }

        if (
            !magic.contentEquals(MAGIC) ||
            (buffer.get().toInt() and 0xff) !=
                VERSION
        ) {
            throw IOException(
                "Invalid protocol header"
            )
        }

        val type =
            buffer.get().toInt() and 0xff

        val length =
            buffer.int

        if (
            length < 0 ||
            length > MAX_PAYLOAD
        ) {
            throw IOException(
                "Invalid message length"
            )
        }

        return Frame(
            type,
            readExact(length)
        )
    }

    fun decodeHex(value: String): ByteArray {
    require(value.length % 2 == 0) {
        "Hex string must have even length"
    }

    return ByteArray(value.length / 2) { index ->
        val high = Character.digit(value[index * 2], 16)
        val low = Character.digit(value[index * 2 + 1], 16)

        require(high >= 0 && low >= 0) {
                "Invalid hexadecimal string"
            }
            
        ((high shl 4) or low).toByte()
        }
    }
}