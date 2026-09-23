#include "network.hpp"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cstring>

std::string getLocalIpAddress()
{
    struct ifaddrs* interfaces = nullptr;

    if (getifaddrs(&interfaces) != 0)
    {
        return {};
    }

    std::string address;

    for (struct ifaddrs* current = interfaces;
         current != nullptr;
         current = current->ifa_next)
    {
        if (current->ifa_addr == nullptr)
        {
            continue;
        }

        if (current->ifa_addr->sa_family != AF_INET)
        {
            continue;
        }

        auto* ipv4 =
            reinterpret_cast<sockaddr_in*>(current->ifa_addr);

        if (ntohl(ipv4->sin_addr.s_addr) == INADDR_LOOPBACK)
        {
            continue;
        }

        char buffer[INET_ADDRSTRLEN]{};

        if (inet_ntop(
                AF_INET,
                &ipv4->sin_addr,
                buffer,
                sizeof(buffer)))
        {
            address = buffer;
            break;
        }
    }

    freeifaddrs(interfaces);

    return address;
}
