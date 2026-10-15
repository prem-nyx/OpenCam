#include "media_pipeline.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

namespace
{
constexpr int OUTPUT_WIDTH = 640;
constexpr int OUTPUT_HEIGHT = 480;
constexpr int OUTPUT_FPS = 30;
constexpr std::int64_t IO_TIMEOUT_US = 3000000;

bool ioctlRetry(int fd, unsigned long request, void* argument)
{
    int result;
    do
    {
        result = ioctl(fd, request, argument);
    } while (result < 0 && errno == EINTR);
    return result >= 0;
}

bool configureV4L2Output(int fd, std::size_t& packedFrameSize,
                         std::size_t& yStride, std::size_t& chromaStride,
                         std::string& error)
{
    v4l2_capability capability{};
    if (!ioctlRetry(fd, VIDIOC_QUERYCAP, &capability))
    {
        error = "V4L2 capability query failed";
        return false;
    }

    const std::uint32_t caps =
        (capability.capabilities & V4L2_CAP_DEVICE_CAPS)
            ? capability.device_caps
            : capability.capabilities;

    if ((caps & V4L2_CAP_VIDEO_OUTPUT) == 0)
    {
        error = "OpenCam V4L2 device does not support video output";
        return false;
    }

    v4l2_format format{};
    format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    format.fmt.pix.width = OUTPUT_WIDTH;
    format.fmt.pix.height = OUTPUT_HEIGHT;
    format.fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420;
    format.fmt.pix.field = V4L2_FIELD_NONE;
    format.fmt.pix.bytesperline = OUTPUT_WIDTH;
    format.fmt.pix.sizeimage =
        OUTPUT_WIDTH * OUTPUT_HEIGHT * 3 / 2;
    format.fmt.pix.colorspace = V4L2_COLORSPACE_SMPTE170M;

    if (!ioctlRetry(fd, VIDIOC_S_FMT, &format))
    {
        error = "OpenCam V4L2 device rejected YUV420 640x480 format";
        return false;
    }

    if (format.fmt.pix.width != OUTPUT_WIDTH ||
        format.fmt.pix.height != OUTPUT_HEIGHT ||
        format.fmt.pix.pixelformat != V4L2_PIX_FMT_YUV420)
    {
        error = "OpenCam V4L2 device negotiated an incompatible format";
        return false;
    }

    yStride = format.fmt.pix.bytesperline;
    if (yStride < OUTPUT_WIDTH)
        yStride = OUTPUT_WIDTH;

    // Planar YUV420 requires even chroma stride. v4l2loopback normally
    // returns a tightly packed stride, but support a padded luma stride.
    if ((yStride & 1U) != 0)
    {
        error = "OpenCam V4L2 device returned an unsupported row stride";
        return false;
    }

    chromaStride = yStride / 2;
    packedFrameSize = yStride * OUTPUT_HEIGHT +
                      2 * chromaStride * (OUTPUT_HEIGHT / 2);

    if (format.fmt.pix.sizeimage != 0 &&
        format.fmt.pix.sizeimage < packedFrameSize)
    {
        error = "OpenCam V4L2 device reported an undersized frame buffer";
        return false;
    }

    v4l2_streamparm parameters{};
    parameters.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    parameters.parm.output.timeperframe.numerator = 1;
    parameters.parm.output.timeperframe.denominator = OUTPUT_FPS;
    // Frame-rate negotiation is optional for some loopback configurations.
    (void)ioctlRetry(fd, VIDIOC_S_PARM, &parameters);
    return true;
}

bool writePackedYuv420(int fd, const AVFrame* frame,
                       std::size_t yStride, std::size_t chromaStride,
                       std::size_t packedFrameSize,
                       const std::atomic<bool>& stopRequested,
                       std::string& error)
{
    std::vector<std::uint8_t> packed(packedFrameSize);
    std::uint8_t* y = packed.data();
    std::uint8_t* u = y + yStride * OUTPUT_HEIGHT;
    std::uint8_t* v = u + chromaStride * (OUTPUT_HEIGHT / 2);

    for (int row = 0; row < OUTPUT_HEIGHT; ++row)
    {
        std::memcpy(y + static_cast<std::size_t>(row) * yStride,
                    frame->data[0] + row * frame->linesize[0],
                    OUTPUT_WIDTH);
        if (yStride > OUTPUT_WIDTH)
            std::memset(y + static_cast<std::size_t>(row) * yStride +
                            OUTPUT_WIDTH,
                        0, yStride - OUTPUT_WIDTH);
    }

    for (int row = 0; row < OUTPUT_HEIGHT / 2; ++row)
    {
        std::memcpy(u + static_cast<std::size_t>(row) * chromaStride,
                    frame->data[1] + row * frame->linesize[1],
                    OUTPUT_WIDTH / 2);
        std::memcpy(v + static_cast<std::size_t>(row) * chromaStride,
                    frame->data[2] + row * frame->linesize[2],
                    OUTPUT_WIDTH / 2);
        if (chromaStride > OUTPUT_WIDTH / 2)
        {
            std::memset(u + static_cast<std::size_t>(row) * chromaStride +
                            OUTPUT_WIDTH / 2,
                        0, chromaStride - OUTPUT_WIDTH / 2);
            std::memset(v + static_cast<std::size_t>(row) * chromaStride +
                            OUTPUT_WIDTH / 2,
                        0, chromaStride - OUTPUT_WIDTH / 2);
        }
    }

    while (!stopRequested.load())
    {
        const ssize_t written = write(fd, packed.data(), packed.size());
        if (written == static_cast<ssize_t>(packed.size()))
            return true;

        if (written < 0 && errno == EINTR)
            continue;

        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            pollfd outputPoll{fd, POLLOUT, 0};
            const int ready = poll(&outputPoll, 1, 100);
            if (ready < 0 && errno != EINTR)
            {
                error = "OpenCam V4L2 output polling failed";
                return false;
            }
            continue;
        }

        error = "OpenCam V4L2 frame write failed or was incomplete";
        return false;
    }

    return false;
}

std::string ffmpegError(int code)
{
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};
    if (av_strerror(code, buffer, sizeof(buffer)) < 0)
        return "unknown FFmpeg error";
    return buffer;
}
} // namespace

MediaPipeline::~MediaPipeline()
{
    stop();
}

bool MediaPipeline::start(const std::string& authenticatedRtspUrl,
                          const std::string& videoDevice)
{
    if (worker_.joinable() ||
        authenticatedRtspUrl.empty() ||
        videoDevice.empty())
        return false;

    stopRequested_.store(false);
    {
        std::lock_guard<std::mutex> lock(errorMutex_);
        errorMessage_.clear();
    }
    state_.store(MediaPipelineState::Starting);
    worker_ = std::thread(&MediaPipeline::run, this,
                          authenticatedRtspUrl, videoDevice);
    return true;
}

void MediaPipeline::stop()
{
    stopRequested_.store(true);
    if (worker_.joinable())
        worker_.join();

    const auto current = state_.load();
    if (current != MediaPipelineState::Failed)
        state_.store(MediaPipelineState::Stopped);
}

MediaPipelineState MediaPipeline::state() const noexcept
{
    return state_.load();
}

std::string MediaPipeline::errorMessage() const
{
    std::lock_guard<std::mutex> lock(errorMutex_);
    return errorMessage_;
}

void MediaPipeline::setFailure(const std::string& message)
{
    {
        std::lock_guard<std::mutex> lock(errorMutex_);
        errorMessage_ = message;
    }
    state_.store(MediaPipelineState::Failed);
}

int MediaPipeline::interruptCallback(void* opaque)
{
    const auto* pipeline = static_cast<const MediaPipeline*>(opaque);
    return pipeline->stopRequested_.load() ? 1 : 0;
}

void MediaPipeline::run(std::string authenticatedRtspUrl,
                        std::string videoDevice)
{
    // Never print FFmpeg's internal messages: protocol diagnostics can include
    // the authenticated URL. Application errors below are deliberately generic.
    av_log_set_level(AV_LOG_QUIET);
    if (avformat_network_init() < 0)
    {
        setFailure("FFmpeg network initialization failed");
        return;
    }

    AVFormatContext* format = avformat_alloc_context();
    if (!format)
    {
        setFailure("Could not allocate RTSP input context");
        avformat_network_deinit();
        return;
    }

    format->interrupt_callback.callback = &MediaPipeline::interruptCallback;
    format->interrupt_callback.opaque = this;

    AVDictionary* options = nullptr;
    av_dict_set(&options, "rtsp_transport", "tcp", 0);
    av_dict_set(&options, "protocol_whitelist", "rtsp,tcp", 0);
    av_dict_set(&options, "rw_timeout", "3000000", 0);
    av_dict_set(&options, "stimeout", "3000000", 0);
    av_dict_set(&options, "analyzeduration", "1000000", 0);

    int result = avformat_open_input(&format, authenticatedRtspUrl.c_str(),
                                     nullptr, &options);
    av_dict_free(&options);
    if (result < 0)
    {
        if (!stopRequested_.load())
            setFailure("Could not open authenticated RTSP source (" +
                       ffmpegError(result) + ")");
        if (format)
            avformat_close_input(&format);
        avformat_network_deinit();
        return;
    }

    result = avformat_find_stream_info(format, nullptr);
    if (result < 0)
    {
        if (!stopRequested_.load())
            setFailure("Could not read RTSP stream metadata (" +
                       ffmpegError(result) + ")");
        avformat_close_input(&format);
        avformat_network_deinit();
        return;
    }

    int videoStreamIndex = -1;
    for (unsigned int i = 0; i < format->nb_streams; ++i)
    {
        if (format->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            videoStreamIndex = static_cast<int>(i);
            break;
        }
    }
    if (videoStreamIndex < 0)
    {
        setFailure("RTSP source contains no video stream");
        avformat_close_input(&format);
        avformat_network_deinit();
        return;
    }

    const AVCodec* decoder =
        avcodec_find_decoder(format->streams[videoStreamIndex]->codecpar->codec_id);
    AVCodecContext* decoderContext = decoder ? avcodec_alloc_context3(decoder) : nullptr;
    if (!decoderContext ||
        avcodec_parameters_to_context(
            decoderContext, format->streams[videoStreamIndex]->codecpar) < 0 ||
        avcodec_open2(decoderContext, decoder, nullptr) < 0)
    {
        setFailure("Could not initialize the RTSP video decoder");
        avcodec_free_context(&decoderContext);
        avformat_close_input(&format);
        avformat_network_deinit();
        return;
    }

    const int videoFd = open(videoDevice.c_str(), O_WRONLY | O_CLOEXEC | O_NONBLOCK);
    if (videoFd < 0)
    {
        setFailure("Could not open the OpenCam V4L2 output device");
        avcodec_free_context(&decoderContext);
        avformat_close_input(&format);
        avformat_network_deinit();
        return;
    }

    std::size_t packedFrameSize = 0;
    std::size_t yStride = 0;
    std::size_t chromaStride = 0;
    std::string v4l2Error;
    if (!configureV4L2Output(videoFd, packedFrameSize, yStride,
                             chromaStride, v4l2Error))
    {
        setFailure(v4l2Error);
        close(videoFd);
        avcodec_free_context(&decoderContext);
        avformat_close_input(&format);
        avformat_network_deinit();
        return;
    }

    AVFrame* decodedFrame = av_frame_alloc();
    AVFrame* convertedFrame = av_frame_alloc();
    AVPacket* packet = av_packet_alloc();
    if (!decodedFrame || !convertedFrame || !packet)
    {
        setFailure("Could not allocate video frame buffers");
        av_packet_free(&packet);
        av_frame_free(&decodedFrame);
        av_frame_free(&convertedFrame);
        close(videoFd);
        avcodec_free_context(&decoderContext);
        avformat_close_input(&format);
        avformat_network_deinit();
        return;
    }

    convertedFrame->format = AV_PIX_FMT_YUV420P;
    convertedFrame->width = OUTPUT_WIDTH;
    convertedFrame->height = OUTPUT_HEIGHT;
    if (av_frame_get_buffer(convertedFrame, 32) < 0)
    {
        setFailure("Could not allocate converted video frame");
        av_packet_free(&packet);
        av_frame_free(&decodedFrame);
        av_frame_free(&convertedFrame);
        close(videoFd);
        avcodec_free_context(&decoderContext);
        avformat_close_input(&format);
        avformat_network_deinit();
        return;
    }

    SwsContext* scaler = nullptr;
    state_.store(MediaPipelineState::Running);
    bool successful = true;
    std::string processingError;

    auto drainDecodedFrames = [&]() -> bool
    {
        while (true)
        {
            const int receiveResult =
                avcodec_receive_frame(decoderContext, decodedFrame);
            if (receiveResult == AVERROR(EAGAIN) ||
                receiveResult == AVERROR_EOF)
                return true;
            if (receiveResult < 0)
            {
                processingError = "Video decoder failed";
                return false;
            }

            scaler = sws_getCachedContext(
                scaler,
                decodedFrame->width,
                decodedFrame->height,
                static_cast<AVPixelFormat>(decodedFrame->format),
                OUTPUT_WIDTH,
                OUTPUT_HEIGHT,
                AV_PIX_FMT_YUV420P,
                SWS_BILINEAR,
                nullptr, nullptr, nullptr);
            if (!scaler || av_frame_make_writable(convertedFrame) < 0)
            {
                processingError = "Could not prepare converted video frame";
                av_frame_unref(decodedFrame);
                return false;
            }

            const int scaledRows =
                sws_scale(scaler, decodedFrame->data, decodedFrame->linesize,
                          0, decodedFrame->height,
                          convertedFrame->data, convertedFrame->linesize);
            if (scaledRows != OUTPUT_HEIGHT)
            {
                processingError = "Video frame conversion returned an incomplete frame";
                av_frame_unref(decodedFrame);
                return false;
            }

            if (!writePackedYuv420(videoFd, convertedFrame, yStride,
                                   chromaStride, packedFrameSize,
                                   stopRequested_, processingError))
            {
                av_frame_unref(decodedFrame);
                return stopRequested_.load();
            }
            av_frame_unref(decodedFrame);
        }
    };

    while (!stopRequested_.load())
    {
        result = av_read_frame(format, packet);
        if (result < 0)
        {
            if (!stopRequested_.load())
            {
                processingError = (result == AVERROR_EOF)
                    ? "RTSP source ended"
                    : "RTSP read failed (" + ffmpegError(result) + ")";
                successful = false;
            }
            break;
        }

        if (packet->stream_index == videoStreamIndex)
        {
            // FFmpeg requires pending decoded frames to be drained before a
            // packet can be retried after EAGAIN.
            while ((result = avcodec_send_packet(decoderContext, packet)) ==
                   AVERROR(EAGAIN))
            {
                if (!drainDecodedFrames())
                {
                    successful = false;
                    break;
                }
            }

            if (successful && result < 0)
            {
                processingError = "Video decoder rejected an input packet";
                successful = false;
            }
            if (successful && !drainDecodedFrames())
                successful = false;
        }

        av_packet_unref(packet);
        if (!successful)
            break;
    }

    sws_freeContext(scaler);
    av_packet_free(&packet);
    av_frame_free(&decodedFrame);
    av_frame_free(&convertedFrame);
    close(videoFd);
    avcodec_free_context(&decoderContext);
    avformat_close_input(&format);
    avformat_network_deinit();

    if (!stopRequested_.load() && !successful)
        setFailure(processingError.empty() ? "Media pipeline failed" : processingError);
    else if (!stopRequested_.load())
        setFailure("Media pipeline stopped unexpectedly");
}
