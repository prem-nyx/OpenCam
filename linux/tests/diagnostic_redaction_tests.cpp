#include "diagnostic_redaction.hpp"

#include <cassert>
#include <string>

int main()
{
    const std::string secret = "SUPER_SECRET_TEST_VALUE";
    const std::string input =
        "[rtsp @ 0x1] failed URL rtsp://opencam:" + secret +
        "@10.0.0.2:8554/live/abc123: connection refused\n";
    const std::string output = redactRtspCredentials(input);
    assert(output.find(secret) == std::string::npos);
    assert(output.find("rtsp://[REDACTED]@10.0.0.2:8554/live/abc123") != std::string::npos);

    const std::string noUserinfo =
        "Could not open rtsp://10.0.0.2:8554/live/abc123: timeout";
    assert(redactRtspCredentials(noUserinfo) == noUserinfo);

    const std::string repeated =
        "first rtsp://user:" + secret + "@host:8554/a second RTSP://u:pw@host:8554/b\n";
    const std::string redactedRepeated = redactRtspCredentials(repeated);
    assert(redactedRepeated.find(secret) == std::string::npos);
    assert(redactedRepeated.find("u:pw") == std::string::npos);

    // A credential-bearing URL split across pipe reads must not be emitted early.
    DiagnosticLineRedactor split;
    assert(split.consume("failure rtsp://opencam:SUPER_SECRET_").empty());
    const std::string splitOutput = split.consume("TEST_VALUE@host:8554/live\n");
    assert(splitOutput.find("SUPER_SECRET_TEST_VALUE") == std::string::npos);
    assert(splitOutput.find("rtsp://[REDACTED]@host:8554/live") != std::string::npos);

    // A too-long line is discarded, with a bounded replacement only after newline.
    DiagnosticLineRedactor oversized;
    assert(oversized.consume(std::string(DiagnosticLineRedactor::MAX_LINE_BYTES + 10, 'x')).empty());
    const std::string oversizedOutput = oversized.consume("tail\nnext line\n");
    assert(oversizedOutput.find("[diagnostic line omitted: exceeds limit]\n") != std::string::npos);
    assert(oversizedOutput.find("next line\n") != std::string::npos);

    DiagnosticLineRedactor incomplete;
    assert(incomplete.consume("partial rtsp://u:pw@host:8554/live").empty());
    incomplete.finish();
    assert(incomplete.consume("next\n").find("next\n") != std::string::npos);
    return 0;
}
