#include "diagnostic_redaction.hpp"

#include <cctype>
#include <string>

namespace
{

    bool startsWithRtspScheme(const std::string &value, std::size_t pos)
    {
        constexpr char rtspScheme[] = "rtsp://";
        constexpr char rtspsScheme[] = "rtsps://";

        const auto matchesScheme = [&](const char *scheme, std::size_t length)
        {
            if (pos + length > value.size())
                return false;

            for (std::size_t i = 0; i < length; ++i)
            {
                if (std::tolower(static_cast<unsigned char>(value[pos + i])) != scheme[i])
                    return false;
            }
            return true;
        };

        return matchesScheme(rtspsScheme, sizeof(rtspsScheme) - 1) ||
               matchesScheme(rtspScheme, sizeof(rtspScheme) - 1);
    }

    std::string redactCompleteLine(const std::string &line)
    {
        std::string output;
        output.reserve(line.size());
        std::size_t cursor = 0;
        while (cursor < line.size())
        {
            std::size_t schemePos = std::string::npos;
            for (std::size_t i = cursor; i + 7 <= line.size(); ++i)
            {
                if (startsWithRtspScheme(line, i))
                {
                    schemePos = i;
                    break;
                }
            }
            if (schemePos == std::string::npos)
            {
                output.append(line, cursor, std::string::npos);
                break;
            }

            output.append(line, cursor, schemePos - cursor);
            const bool isRtsps =
                schemePos + 8 <= line.size() &&
                std::tolower(static_cast<unsigned char>(line[schemePos + 4])) == 's' &&
                startsWithRtspScheme(line, schemePos);

            const std::size_t schemeLength = isRtsps ? 8 : 7;
            output.append(line, schemePos, schemeLength);
            std::size_t authorityEnd = line.find_first_of(
                "/?# \t\r\n\"'<>)]}", schemePos + schemeLength);
            if (authorityEnd == std::string::npos)
                authorityEnd = line.size();

            const std::size_t authorityStart = schemePos + schemeLength;
            const std::size_t at = line.rfind('@', authorityEnd);
            if (at != std::string::npos && at >= authorityStart && at < authorityEnd)
            {
                output += "[REDACTED]@";
                output.append(line, at + 1, authorityEnd - at - 1);
            }
            else
            {
                output.append(line, authorityStart, authorityEnd - authorityStart);
            }
            cursor = authorityEnd;
        }
        return output;
    }

    std::string sanitize(const std::string &input)
    {
        std::string output;
        output.reserve(input.size());
        for (unsigned char c : input)
        {
            if (c == '\n' || c == '\r' || c == '\t' || (c >= 0x20 && c <= 0x7e))
                output.push_back(static_cast<char>(c));
            else
                output.push_back('?');
        }
        return output;
    }
}

std::string redactRtspCredentials(const std::string &input)
{
    return redactCompleteLine(input);
}

std::string DiagnosticLineRedactor::consume(const std::string &chunk)
{
    std::string output;
    for (char c : chunk)
    {
        if (discarding_)
        {
            if (c == '\n')
            {
                discarding_ = false;
                output += "[diagnostic line omitted: exceeds limit]\n";
            }
            continue;
        }

        pending_.push_back(c);
        if (c == '\n')
        {
            output += sanitize(redactCompleteLine(pending_));
            pending_.clear();
        }
        else if (pending_.size() > MAX_LINE_BYTES)
        {
            pending_.clear();
            discarding_ = true;
        }
    }
    return output;
}

void DiagnosticLineRedactor::finish()
{
    // Never emit an incomplete line: it may contain a URL whose tail has not
    // arrived yet, and logging partial data is not necessary for diagnosis.
    pending_.clear();
    discarding_ = false;
}
