#include "security_validation.hpp"

#include <arpa/inet.h>

bool isAllowedRtspEndpoint(const std::string &url, const std::string &peerIp)
{
    constexpr char rtspPrefix[] = "rtsp://";
    constexpr char rtspsPrefix[] = "rtsps://";

    std::size_t prefixLength = 0;

    if (url.rfind(rtspsPrefix, 0) == 0)
        prefixLength = sizeof(rtspsPrefix) - 1;
    else if (url.rfind(rtspPrefix, 0) == 0)
        prefixLength = sizeof(rtspPrefix) - 1;
    else
        return false;

    const std::string endpoint = url.substr(prefixLength);
    const auto slash = endpoint.find('/');
    if (slash == std::string::npos)
        return false;
    const std::string path = endpoint.substr(slash);
    if (path.size() != 38 || path.rfind("/live/", 0) != 0)
        return false;
    for (std::size_t i = 6; i < path.size(); ++i)
        if (!((path[i] >= '0' && path[i] <= '9') ||
              (path[i] >= 'a' && path[i] <= 'f')))
            return false;
    const std::string authority = endpoint.substr(0, slash);
    if (authority.find('@') != std::string::npos || authority.find('?') != std::string::npos ||
        authority.find('#') != std::string::npos)
        return false;
    const auto colon = authority.rfind(':');
    if (colon == std::string::npos || authority.substr(colon + 1) != "8554")
        return false;
    const std::string host = authority.substr(0, colon);
    in_addr parsed{};
    return inet_pton(AF_INET, host.c_str(), &parsed) == 1 && host == peerIp;
}
