#include "qr.hpp"

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <iostream>

bool displayPairingQr(const std::string& payload)
{
    const pid_t pid = fork();

    if (pid < 0)
    {
        return false;
    }

    if (pid == 0)
    {
        execlp(
            "qrencode",
            "qrencode",
            "-t",
            "ANSIUTF8",
            "-o",
            "-",
            payload.c_str(),
            static_cast<char*>(nullptr)
        );

        _exit(127);
    }

    int status = 0;

    if (waitpid(pid, &status, 0) < 0)
    {
        return false;
    }

    return WIFEXITED(status) &&
           WEXITSTATUS(status) == 0;
}
