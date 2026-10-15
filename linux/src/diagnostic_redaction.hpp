#pragma once

#include <cstddef>
#include <string>

// Removes RTSP URL userinfo from diagnostic text. Defense in depth only:
// it does not protect credentials passed through process argv.
std::string redactRtspCredentials(const std::string &input);

// Buffers diagnostic chunks until complete lines are available so a URL split
// across read() calls cannot bypass redaction. Oversized lines are discarded.
class DiagnosticLineRedactor
{
public:
    static constexpr std::size_t MAX_LINE_BYTES = 8192;

    std::string consume(const std::string &chunk);
    void finish();

private:
    std::string pending_;
    bool discarding_ = false;
};
