package com.darusc.vcamdroid

import android.content.Intent
import android.os.Bundle
import android.view.SurfaceHolder
import android.view.WindowManager
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import com.darusc.vcamdroid.databinding.ActivityStreamBinding
import com.darusc.vcamdroid.networking.ConnectionManager
import com.darusc.vcamdroid.networking.PacketType
import com.darusc.vcamdroid.rtsp.Streamer
import com.darusc.vcamdroid.rtsp.StreamOptions
import com.darusc.vcamdroid.util.Logger
import java.io.ByteArrayOutputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.charset.CodingErrorAction
import java.nio.charset.StandardCharsets

class StreamActivity : AppCompatActivity(), SurfaceHolder.Callback, ConnectionManager.ConnectionStateCallback {

    private val TAG = "VCamdroid"

    private lateinit var viewBinding: ActivityStreamBinding

    private val connectionManager = ConnectionManager.getInstance(this)
    private lateinit var streamer: Streamer
    private val legacyPending = ByteArrayOutputStream()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        viewBinding = ActivityStreamBinding.inflate(layoutInflater)
        viewBinding.surfaceView.holder.addCallback(this)

        setContentView(viewBinding.root)

        // Disable navigating to logs activity for now as it breaks streaming
//        viewBinding.logReportButton.setOnClickListener {
//            val intent = Intent(this, LogActivity::class.java)
//            startActivity(intent)
//        }

        connectionManager.setOnFrameReceivedCallback(::onFrameReceived)
        connectionManager.setOnLegacyBytesReceivedCallback(::onLegacyBytesReceived)
        streamer = Streamer(StreamOptions(), this, viewBinding.surfaceView)
    }

    override fun surfaceCreated(holder: SurfaceHolder) {
        if (holder.surface != null && holder.surface.isValid) {
            streamer.startPreview()
        }
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) { }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        streamer.stop()
    }

    override fun onDisconnected() {
        Toast.makeText(this, "Connection closed by server", Toast.LENGTH_LONG).show()
        Logger.log("STREAM", "TCP server disconnected")
        streamer.stop()
        finish()
    }

    private fun onFrameReceived(type: Int, payload: ByteArray) {
        try {
            require(type == com.darusc.vcamdroid.networking.ControlProtocol.TYPE_ACTIVATION)
            handlePacket(PacketType.ACTIVATION.toInt(), payload)
        } catch (_: Exception) {
            Logger.log("STREAM", "Rejected malformed authenticated control message")
            connectionManager.close()
            runOnUiThread { onDisconnected() }
        }
    }

    private fun onLegacyBytesReceived(buffer: ByteArray, bytes: Int) {
        try {
            require(bytes in 1..buffer.size && legacyPending.size() + bytes <= 65536)
            legacyPending.write(buffer, 0, bytes)
            while (true) {
                val pending = legacyPending.toByteArray()
                val length = legacyMessageLength(pending) ?: return
                require(length in 1..pending.size)
                handlePacket(pending[0].toInt() and 0xff, pending.copyOfRange(1, length))
                legacyPending.reset()
                legacyPending.write(pending, length, pending.size - length)
            }
        } catch (_: Exception) {
            Logger.log("STREAM", "Rejected malformed legacy control message")
            connectionManager.close()
            runOnUiThread { onDisconnected() }
        }
    }

    private fun legacyMessageLength(data: ByteArray): Int? {
        if (data.isEmpty()) return null
        fun need(size: Int) { require(size <= 65536) }
        val type = data[0].toInt() and 0xff
        val fixed = when (type) {
            PacketType.CAMERA.toInt() -> 1
            PacketType.RESOLUTION.toInt() -> 5
            PacketType.ROTATION.toInt(), PacketType.FOCUS.toInt(), PacketType.FPS.toInt(),
            PacketType.FLASH.toInt(), PacketType.CODEC.toInt(), PacketType.STABILIZATION.toInt(),
            PacketType.FLIP.toInt() -> 2
            PacketType.BITRATE.toInt() -> 3
            PacketType.ADAPTIVE_BITRATE.toInt(), PacketType.ZOOM.toInt() -> 5
            PacketType.EFFECT_FILTER.toInt(), PacketType.CORRECTION_FILTER.toInt() -> {
                if (data.size < 2) return null
                val nameLength = data[1].toInt() and 0xff
                require(nameLength <= 128)
                2 + nameLength + if (type == PacketType.CORRECTION_FILTER.toInt()) 1 else 0
            }
            PacketType.ACTIVATION.toInt() -> {
                if (data.size < 39) return null
                val view = ByteBuffer.wrap(data).order(ByteOrder.BIG_ENDIAN)
                view.position(37)
                val filterCount = view.short.toInt() and 0xffff
                require(filterCount <= 128)
                var offset = 39
                repeat(filterCount) {
                    if (data.size < offset + 2) return null
                    val nameLength = ((data[offset].toInt() and 0xff) shl 8) or (data[offset + 1].toInt() and 0xff)
                    require(nameLength <= 128)
                    offset += 2 + nameLength + 4
                    if (data.size < offset) return null
                }
                if (data.size < offset + 2) return null
                val effectLength = ((data[offset].toInt() and 0xff) shl 8) or (data[offset + 1].toInt() and 0xff)
                require(effectLength <= 128)
                offset + 2 + effectLength
            }
            else -> throw IllegalArgumentException("Unknown legacy command")
        }
        need(fixed)
        return if (data.size < fixed) null else fixed
    }

    private fun strictUtf8(bytes: ByteArray): String = StandardCharsets.UTF_8.newDecoder()
        .onMalformedInput(CodingErrorAction.REPORT)
        .onUnmappableCharacter(CodingErrorAction.REPORT)
        .decode(ByteBuffer.wrap(bytes)).toString()

    private fun handlePacket(type: Int, payload: ByteArray) {
        fun requireSize(size: Int) { require(payload.size == size) }
        fun u16(offset: Int) = (payload[offset].toInt() and 0xff) or ((payload[offset + 1].toInt() and 0xff) shl 8)
        when (type) {
            PacketType.ACTIVATION.toInt() -> streamer.startStream(StreamOptions.deserialize(payload))
            PacketType.CAMERA.toInt() -> { requireSize(0); streamer.switchCamera() }
            PacketType.RESOLUTION.toInt() -> {
                requireSize(4); val width = u16(0); val height = u16(2)
                require(width in 160..8192 && height in 160..8192 && width.toLong() * height <= 16_777_216L)
                streamer.setResolution(width, height)
            }
            PacketType.ROTATION.toInt() -> { requireSize(1); streamer.rotate(payload[0].toInt() and 0xff) }
            PacketType.EFFECT_FILTER.toInt(), PacketType.CORRECTION_FILTER.toInt() -> {
                require(payload.isNotEmpty())
                val nameLength = payload[0].toInt() and 0xff
                val correction = type == PacketType.CORRECTION_FILTER.toInt()
                require(nameLength in 1..128 && payload.size == 1 + nameLength + if (correction) 1 else 0)
                val name = strictUtf8(payload.copyOfRange(1, 1 + nameLength))
                if (correction) streamer.applyCorrectionFilter(name, payload.last().toInt())
                else streamer.applyEffectFilter(name)
            }
            PacketType.BITRATE.toInt() -> { requireSize(2); val value = u16(0); require(value in 64..25000); streamer.setBitrate(value) }
            PacketType.ADAPTIVE_BITRATE.toInt() -> { requireSize(4); val min = u16(0); val max = u16(2); require(min in 64..25000 && max in min..25000); streamer.setAdaptiveBitrate(min, max) }
            PacketType.STABILIZATION.toInt(), PacketType.FLASH.toInt(), PacketType.CODEC.toInt() -> {
                requireSize(1); require(payload[0].toInt() == 0 || payload[0].toInt() == 1)
                val value = payload[0].toInt() == 1
                when (type) { PacketType.STABILIZATION.toInt() -> streamer.setStabilization(value); PacketType.FLASH.toInt() -> streamer.setFlash(value); else -> streamer.setH265Codec(value) }
            }
            PacketType.FOCUS.toInt() -> { requireSize(1); val value = payload[0].toInt() and 0xff; require(value in 0..1); streamer.setFocus(value) }
            PacketType.FPS.toInt() -> { requireSize(1); val value = payload[0].toInt() and 0xff; require(value in 1..60); streamer.setFps(value) }
            PacketType.ZOOM.toInt() -> { requireSize(4); val value = ByteBuffer.wrap(payload).order(ByteOrder.LITTLE_ENDIAN).float; require(value.isFinite() && value in 1.0f..10.0f); streamer.setZoom(value) }
            PacketType.FLIP.toInt() -> { requireSize(1); val axis = payload[0].toInt(); require(axis in 0..1); streamer.flip(if (axis == 0) StreamOptions.FlipAxis.VERTICAL else StreamOptions.FlipAxis.HORIZONTAL) }
            else -> throw IllegalArgumentException("Unknown command")
        }
    }
}
