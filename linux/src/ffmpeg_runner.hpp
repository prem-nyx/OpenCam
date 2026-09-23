#pragma once

#include <sys/types.h>
#include <string>

pid_t startFfmpeg(
    const std::string& rtspUrl,
    const std::string& videoDevice
);

bool stopFfmpeg(pid_t pid);