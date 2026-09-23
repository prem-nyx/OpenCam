#include "rtsp_probe.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>

bool waitForRtsp(const std::string& rtspUrl)
{
    constexpr int maxAttempts = 10;
    constexpr int retryDelayMs = 500;

    for (int attempt = 1; attempt <= maxAttempts; ++attempt)
    {
        std::cout
            << "Checking RTSP server (attempt "
            << attempt << "/"
            << maxAttempts
            << ")...\n";

        const std::string command =
            "ffprobe -v error "
            "-rtsp_transport tcp "
            "-select_streams v:0 "
            "-show_entries stream=codec_name "
            "-of default=noprint_wrappers=1:nokey=1 "
            "\"" + rtspUrl + "\" "
            "> /dev/null 2>&1";

        if (std::system(command.c_str()) == 0)
        {
            std::cout << "RTSP server is ready.\n";
            return true;
        }

        std::this_thread::sleep_for(
            std::chrono::milliseconds(retryDelayMs)
        );
    }

    std::cerr << "RTSP server did not become ready.\n";
    return false;
}
