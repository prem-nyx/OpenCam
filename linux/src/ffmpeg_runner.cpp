#include "ffmpeg_runner.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <iostream>
#include <linux/close_range.h>
#include <string>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace
{
    constexpr std::size_t MAX_DIAGNOSTIC_BYTES = 8192;

    void closeIfValid(int fd)
    {
        if (fd >= 0)
            close(fd);
    }

    std::string sanitizeDiagnostics(const std::string &input)
    {
        std::string output;
        output.reserve(input.size());

        // Keep diagnostics bounded even if FFmpeg produces unexpectedly large
        // output before the parent gets a chance to drain the pipe.
        const std::size_t limit =
            std::min(input.size(), MAX_DIAGNOSTIC_BYTES);

        for (std::size_t i = 0; i < limit; ++i)
        {
            const unsigned char c =
                static_cast<unsigned char>(input[i]);

            if (c == '\n' || c == '\r' || c == '\t')
            {
                output.push_back(static_cast<char>(c));
            }
            else if (c >= 0x20 && c <= 0x7e)
            {
                output.push_back(static_cast<char>(c));
            }
            else
            {
                output.push_back('?');
            }
        }

        return output;
    }
}

FfmpegProcess startFfmpeg(
    const std::string &rtspUrl,
    const std::string &videoDevice)
{
    int stderrPipe[2]{};
    if (pipe2(stderrPipe, O_CLOEXEC | O_NONBLOCK) < 0)
    {
        std::cerr << "Failed to create FFmpeg diagnostic pipe.\n";
        return {};
    }

    const pid_t pid = fork();

    if (pid < 0)
    {
        close(stderrPipe[0]);
        close(stderrPipe[1]);
        std::cerr << "Failed to fork FFmpeg process.\n";
        return {};
    }

    if (pid == 0)
    {
        // The child must not retain the parent's read end.
        close(stderrPipe[0]);

        if (dup2(stderrPipe[1], STDERR_FILENO) < 0)
            _exit(127);

        close(stderrPipe[1]);

        if (syscall(SYS_close_range, 3U, ~0U, 0U) < 0)
        {
            const long maxFd =
                std::min(sysconf(_SC_OPEN_MAX), 65536L);

            for (int fd = 3; fd < maxFd; ++fd)
                close(fd);
        }

        const int nullFd = open("/dev/null", O_RDWR);
        if (nullFd >= 0)
        {
            dup2(nullFd, STDIN_FILENO);
            dup2(nullFd, STDOUT_FILENO);

            if (nullFd > STDOUT_FILENO)
                close(nullFd);
        }

        execlp(
            "ffmpeg",
            "ffmpeg",
            "-hide_banner",
            "-loglevel",
            "error",
            "-rtsp_transport",
            "tcp",
            "-protocol_whitelist",
            "rtsp,tcp",
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
            static_cast<char *>(nullptr));

        _exit(127);
    }

    // Parent keeps only the read end.
    close(stderrPipe[1]);

    FfmpegProcess process;
    process.pid = pid;
    process.stderrFd = stderrPipe[0];
    return process;
}

std::string drainFfmpegStderr(FfmpegProcess &process)
{
    if (process.stderrFd < 0)
        return {};

    std::string output;
    output.reserve(1024);

    char buffer[1024];

    while (output.size() < MAX_DIAGNOSTIC_BYTES)
    {
        const ssize_t count =
            read(process.stderrFd, buffer, sizeof(buffer));

        if (count > 0)
        {
            const std::size_t remaining =
                MAX_DIAGNOSTIC_BYTES - output.size();

            output.append(
                buffer,
                static_cast<std::size_t>(
                    std::min<ssize_t>(count, remaining)));

            continue;
        }

        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            break;

        if (count < 0 && errno == EINTR)
            continue;

        break;
    }

    return sanitizeDiagnostics(output);
}

bool stopFfmpeg(FfmpegProcess &process)
{
    if (process.pid <= 0)
    {
        closeIfValid(process.stderrFd);
        process.stderrFd = -1;
        return false;
    }

    if (kill(process.pid, SIGTERM) < 0 && errno != ESRCH)
        return false;

    int status = 0;

    const auto deadline =
        std::chrono::steady_clock::now() +
        std::chrono::seconds(2);

    while (std::chrono::steady_clock::now() < deadline)
    {
        const pid_t result =
            waitpid(process.pid, &status, WNOHANG);

        if (result == process.pid ||
            (result < 0 && errno == ECHILD))
        {
            closeIfValid(process.stderrFd);
            process.stderrFd = -1;
            process.pid = -1;
            return true;
        }

        if (result < 0 && errno != EINTR)
            break;

        std::this_thread::sleep_for(
            std::chrono::milliseconds(20));
    }

    if (kill(process.pid, SIGKILL) < 0 && errno != ESRCH)
        return false;

    while (waitpid(process.pid, &status, 0) < 0)
    {
        if (errno != EINTR && errno != ECHILD)
            return false;
    }

    closeIfValid(process.stderrFd);
    process.stderrFd = -1;
    process.pid = -1;

    return true;
}