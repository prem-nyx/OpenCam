#pragma once

#include <cstdint>
#include <vector>

struct StreamOptions
{
    std::int32_t fps = 30;

    std::int32_t width = 640;
    std::int32_t height = 480;

    std::int32_t camera = 1; // 1 = BACK, 0 = FRONT

    bool adaptiveBitrate = false;

    std::int32_t bitrate = 4096000;
    std::int32_t minBitrate = 512000;
    std::int32_t maxBitrate = 25600000;

    bool stabilization = false;
    bool flash = false;
    bool h265 = false;

    std::int32_t focusMode = 0;
};

std::vector<std::uint8_t> buildActivationPacket(
    const StreamOptions& options);
