#include "rtsp_probe.hpp"

#include <chrono>
#include <iostream>
#include <thread>
#include <string>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
}

namespace
{
struct ProbeDeadline
{
    std::chrono::steady_clock::time_point deadline;
};

int interruptProbe(void* opaque)
{
    const auto* state = static_cast<const ProbeDeadline*>(opaque);
    return std::chrono::steady_clock::now() >= state->deadline ? 1 : 0;
}

bool runProbe(const std::string& url)
{
    av_log_set_level(AV_LOG_QUIET);
    ProbeDeadline deadline{std::chrono::steady_clock::now() +
                           std::chrono::seconds(4)};
    if (avformat_network_init() < 0)
        return false;

    AVFormatContext* format = avformat_alloc_context();
    if (!format)
    {
        avformat_network_deinit();
        return false;
    }
    format->interrupt_callback.callback = interruptProbe;
    format->interrupt_callback.opaque = &deadline;

    AVDictionary* options = nullptr;
    av_dict_set(&options, "rtsp_transport", "tcp", 0);
    av_dict_set(&options, "protocol_whitelist", "rtsp,tcp", 0);
    av_dict_set(&options, "rw_timeout", "3000000", 0);
    av_dict_set(&options, "stimeout", "3000000", 0);
    av_dict_set(&options, "analyzeduration", "1000000", 0);

    int result = avformat_open_input(&format, url.c_str(), nullptr, &options);
    av_dict_free(&options);
    if (result >= 0)
        result = avformat_find_stream_info(format, nullptr);

    bool hasVideo = false;
    if (result >= 0 && format)
    {
        for (unsigned int i = 0; i < format->nb_streams; ++i)
        {
            if (format->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
            {
                hasVideo = true;
                break;
            }
        }
    }

    if (format)
        avformat_close_input(&format);
    avformat_network_deinit();
    return result >= 0 && hasVideo;
}
}

bool waitForRtsp(const std::string& rtspUrl)
{
    constexpr int maxAttempts = 10;
    constexpr int retryDelayMs = 500;

    for (int attempt = 1; attempt <= maxAttempts; ++attempt)
    {
        std::cout << "Checking RTSP source (attempt " << attempt
                  << "/" << maxAttempts << ")...\n";
        if (runProbe(rtspUrl))
        {
            std::cout << "RTSP source is ready.\n";
            return true;
        }
        if (attempt != maxAttempts)
            std::this_thread::sleep_for(std::chrono::milliseconds(retryDelayMs));
    }

    std::cerr << "RTSP source did not become ready.\n";
    return false;
}
