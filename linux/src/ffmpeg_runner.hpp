#pragma once

#include <sys/types.h>
#include <string>

struct FfmpegProcess
{
    pid_t pid = -1;
    int stderrFd = -1;
};

FfmpegProcess startFfmpeg(
    const std::string& rtspUrl,
    const std::string& videoDevice
);

std::string drainFfmpegStderr(
    FfmpegProcess& process
);

bool stopFfmpeg(FfmpegProcess& process);