#include "qr.hpp"

#include <sys/types.h>
#include <sys/wait.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <unistd.h>

#include <iostream>

bool displayPairingQr(const std::string& payload)
{
    int inputPipe[2]{};
    if (pipe2(inputPipe, O_CLOEXEC) < 0) return false;

    const pid_t pid = fork();

    if (pid < 0)
    {
        close(inputPipe[0]);
        close(inputPipe[1]);
        return false;
    }

    if (pid == 0)
    {
        close(inputPipe[1]);
        if (dup2(inputPipe[0], STDIN_FILENO) < 0) _exit(127);
        close(inputPipe[0]);
        execlp(
            "qrencode",
            "qrencode",
            "-t",
            "ANSIUTF8",
            "-o",
            "-",
            static_cast<char*>(nullptr)
        );

        _exit(127);
    }

    close(inputPipe[0]);
    std::size_t offset = 0;
    while (offset < payload.size())
    {
        const ssize_t count = write(inputPipe[1], payload.data() + offset,
                                    payload.size() - offset);
        if (count < 0)
        {
            if (errno == EINTR) continue;
            close(inputPipe[1]);
            kill(pid, SIGTERM);
            waitpid(pid, nullptr, 0);
            return false;
        }
        offset += static_cast<std::size_t>(count);
    }
    close(inputPipe[1]);

    int status = 0;

    if (waitpid(pid, &status, 0) < 0)
    {
        return false;
    }

    return WIFEXITED(status) &&
           WEXITSTATUS(status) == 0;
}
