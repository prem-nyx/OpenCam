#include "ffmpeg_runner.hpp"

#include <csignal>
#include <iostream>
#include <sys/wait.h>
#include <unistd.h>

pid_t startFfmpeg(
    const std::string& rtspUrl,
    const std::string& videoDevice)
{
    const pid_t pid = fork();

    if (pid < 0)
    {
        std::cerr << "Failed to fork FFmpeg process.\n";
        return -1;
    }

    if (pid == 0)
    {
        execlp(
            "ffmpeg",
            "ffmpeg",
            "-hide_banner",
            "-rtsp_transport",
            "tcp",
            "-i",
            rtspUrl.c_str(),
            "-an",
            "-vf",
            "format=yuv420p",
            "-f",
            "v4l2",
            "-pix_fmt",
            "yuv420p",
            "-video_size",
            "640x480",
            videoDevice.c_str(),
            static_cast<char*>(nullptr)
        );

        std::cerr << "Failed to start FFmpeg.\n";
        _exit(127);
    }

    return pid;
}

bool stopFfmpeg(pid_t pid)
{
    if (pid <= 0)
    {
        return false;
    }

    if (kill(pid, SIGTERM) < 0)
    {
        return false;
    }

    int status = 0;

    if (waitpid(pid, &status, 0) < 0)
    {
        return false;
    }

    return true;
}