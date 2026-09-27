#include "rtsp_probe.hpp"

#include <chrono>
#include <algorithm>
#include <csignal>
#include <fcntl.h>
#include <linux/close_range.h>
#include <iostream>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <cerrno>
#include <chrono>
#include <thread>
#include <vector>

namespace
{
    bool runProbe(const std::string& url)
    {
        const pid_t pid = fork();
        if (pid < 0)
        {
            std::cerr << "Could not create ffprobe process.\n";
            return false;
        }
        if (pid == 0)
        {
            const int nullFd = open("/dev/null", O_RDWR);
            if (nullFd >= 0)
            {
                dup2(nullFd, STDIN_FILENO);
                dup2(nullFd, STDOUT_FILENO);
                dup2(nullFd, STDERR_FILENO);
                if (nullFd > STDERR_FILENO) close(nullFd);
            }

            // Do not pass control/listener descriptors into the probe process.
            if (syscall(SYS_close_range, 3U, ~0U, 0U) < 0)
            {
                const long maxFd = std::min(sysconf(_SC_OPEN_MAX), 65536L);
                for (int fd = 3; fd < maxFd; ++fd) close(fd);
            }

            execlp("ffprobe", "ffprobe",
                   "-v", "error",
                   "-protocol_whitelist", "rtsp,tcp",
                   "-rw_timeout", "3000000",
                   "-rtsp_transport", "tcp",
                   "-analyzeduration", "1000000",
                   "-select_streams", "v:0",
                   "-show_entries", "stream=codec_name",
                   "-of", "default=noprint_wrappers=1:nokey=1",
                   url.c_str(), static_cast<char*>(nullptr));
            _exit(127);
        }

        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(8);
        int status = 0;
        while (std::chrono::steady_clock::now() < deadline)
        {
            const pid_t result = waitpid(pid, &status, WNOHANG);
            if (result == pid)
            {
                if (WIFEXITED(status))
                {
                    const int exitCode = WEXITSTATUS(status);
                    if (exitCode != 0)
                        std::cerr << "RTSP probe exited with status " << exitCode << ".\n";
                    return exitCode == 0;
                }
                if (WIFSIGNALED(status))
                    std::cerr << "RTSP probe terminated by signal " << WTERMSIG(status) << ".\n";
                return false;
            }
            if (result < 0 && errno != EINTR) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        std::cerr << "RTSP probe exceeded its eight-second deadline.\n";
        kill(pid, SIGTERM);
        const auto terminateDeadline = std::chrono::steady_clock::now() +
                                      std::chrono::milliseconds(500);
        while (std::chrono::steady_clock::now() < terminateDeadline)
        {
            const pid_t result = waitpid(pid, &status, WNOHANG);
            if (result == pid) return false;
            if (result < 0 && errno != EINTR) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        kill(pid, SIGKILL);
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        return false;
    }
}

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

        if (runProbe(rtspUrl))
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
