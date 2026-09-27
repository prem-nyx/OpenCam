package com.darusc.vcamdroid.rtsp

import com.darusc.vcamdroid.video.filters.FilterAdjuster
import com.darusc.vcamdroid.video.filters.FilterRepository
import com.darusc.vcamdroid.video.filters.custom.HorizontalFlipFilterRender
import com.darusc.vcamdroid.video.filters.custom.VerticalFlipFilterRender
import com.pedro.encoder.input.gl.render.filters.BaseFilterRender
import com.pedro.encoder.input.video.CameraHelper
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.charset.CodingErrorAction
import java.nio.charset.StandardCharsets

data class StreamOptions(
    var camera: CameraHelper.Facing = CameraHelper.Facing.BACK,
    var bitrate: Int = 4000 * 1024,
    var adaptiveBitrateMin: Int = 500 * 1024,
    var adaptiveBitrateMax: Int = 25000 * 1024,
    var adaptiveBitrateEnabled: Boolean = false,
    var width: Int = 640,
    var height: Int = 480,
    var fps: Int = 30,
    var rotation: Int = 0,
    var activeEffectFilter: BaseFilterRender? = null,
    var activeCorrectionFilters: MutableSet<BaseFilterRender> = HashSet(),
    var stabilization: Boolean = false,
    var flashEnabled: Boolean = false,
    var focusMode: Int = 0,
    var h265Enabled: Boolean = false,
    var vFlipFilter: VerticalFlipFilterRender? = null,
    var hFlipFilter: HorizontalFlipFilterRender? = null
) {
    enum class FlipAxis { HORIZONTAL, VERTICAL }

    companion object {
        private const val MAX_PACKET = 64 * 1024
        private const val MAX_FILTERS = 128
        private const val MAX_FILTER_NAME = 128

        fun deserialize(data: ByteArray): StreamOptions {
            require(data.size in 40..MAX_PACKET) { "Invalid activation size" }
            val buffer = ByteBuffer.wrap(data).order(ByteOrder.BIG_ENDIAN)
            fun readString(): String {
                require(buffer.remaining() >= 2) { "Truncated string length" }
                val length = buffer.short.toInt() and 0xffff
                require(length <= MAX_FILTER_NAME && buffer.remaining() >= length) { "Invalid string length" }
                val bytes = ByteArray(length)
                buffer.get(bytes)
                return StandardCharsets.UTF_8.newDecoder()
                    .onMalformedInput(CodingErrorAction.REPORT)
                    .onUnmappableCharacter(CodingErrorAction.REPORT)
                    .decode(ByteBuffer.wrap(bytes)).toString()
            }
            fun readBool(): Boolean {
                require(buffer.hasRemaining()) { "Truncated boolean" }
                return when (buffer.get().toInt() and 0xff) {
                    0 -> false
                    1 -> true
                    else -> throw IllegalArgumentException("Invalid boolean")
                }
            }

            require(buffer.remaining() >= 36) { "Truncated activation fields" }
            val fps = buffer.int
            val width = buffer.int
            val height = buffer.int
            val cameraValue = buffer.int
            require(cameraValue in 0..1) { "Invalid camera value" }
            val adaptiveBitrate = readBool()
            val bitrate = buffer.int
            val minBitrate = buffer.int
            val maxBitrate = buffer.int
            val stabilization = readBool()
            val flash = readBool()
            val h265 = readBool()
            val focusMode = buffer.int

            require(fps in 1..60) { "FPS outside allowed range" }
            require(width in 160..8192 && height in 160..8192 && width.toLong() * height <= 16_777_216L) {
                "Resolution outside allowed range"
            }
            require(bitrate in 64_000..50_000_000 && minBitrate in 64_000..50_000_000 &&
                maxBitrate in 64_000..50_000_000 && minBitrate <= maxBitrate) { "Invalid bitrate" }
            require(focusMode in 0..1) { "Invalid focus mode" }

            require(buffer.remaining() >= 2) { "Missing filter count" }
            val filterCount = buffer.short.toInt() and 0xffff
            require(filterCount <= MAX_FILTERS) { "Too many filters" }
            val filtersMap = mutableSetOf<BaseFilterRender>()
            repeat(filterCount) {
                val name = readString()
                require(buffer.remaining() >= 4) { "Truncated filter value" }
                val value = buffer.int
                require(value in -100..100) { "Invalid filter adjustment" }
                val filter = FilterRepository.create(name) ?: throw IllegalArgumentException("Unknown filter")
                require(FilterRepository.getCategory(name) == FilterRepository.Category.CORRECTION) { "Invalid correction filter" }
                FilterAdjuster.adjust(filter, value)
                filtersMap.add(filter)
            }
            val activeEffectName = readString()
            require(buffer.remaining() == 0) { "Trailing activation bytes" }
            val effect = if (activeEffectName.isEmpty()) null else {
                require(FilterRepository.getCategory(activeEffectName) == FilterRepository.Category.EFFECT) { "Invalid effect filter" }
                FilterRepository.create(activeEffectName) ?: throw IllegalArgumentException("Unknown effect filter")
            }

            return StreamOptions(
                camera = if (cameraValue == 1) CameraHelper.Facing.BACK else CameraHelper.Facing.FRONT,
                bitrate = bitrate,
                adaptiveBitrateMin = minBitrate,
                adaptiveBitrateMax = maxBitrate,
                adaptiveBitrateEnabled = adaptiveBitrate,
                width = width,
                height = height,
                fps = fps,
                activeEffectFilter = effect,
                activeCorrectionFilters = filtersMap,
                stabilization = stabilization,
                flashEnabled = flash,
                focusMode = focusMode,
                h265Enabled = h265
            )
        }
    }
}
