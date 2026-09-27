#include "network.hpp"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cstring>
#include <net/if.h>
#include <unistd.h>

std::string getLocalIpAddress()
{
    // A UDP connect performs route selection but sends no packet. Reading the
    // selected local endpoint avoids getifaddrs enumeration-order guesses.
    const int routeSocket = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (routeSocket >= 0)
    {
        sockaddr_in routeTarget{};
        routeTarget.sin_family = AF_INET;
        routeTarget.sin_port = htons(9);
        inet_pton(AF_INET, "192.0.2.1", &routeTarget.sin_addr);
        if (connect(routeSocket, reinterpret_cast<sockaddr*>(&routeTarget),
                    sizeof(routeTarget)) == 0)
        {
            sockaddr_in selected{};
            socklen_t selectedSize = sizeof(selected);
            char buffer[INET_ADDRSTRLEN]{};
            if (getsockname(routeSocket, reinterpret_cast<sockaddr*>(&selected),
                            &selectedSize) == 0 &&
                inet_ntop(AF_INET, &selected.sin_addr, buffer, sizeof(buffer)))
            {
                close(routeSocket);
                return buffer;
            }
        }
        close(routeSocket);
    }

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
        if (current->ifa_addr == nullptr ||
            (current->ifa_flags & IFF_UP) == 0 ||
            (current->ifa_flags & IFF_LOOPBACK) != 0)
        {
            continue;
        }

        if (current->ifa_addr->sa_family != AF_INET)
        {
            continue;
        }

        auto* ipv4 =
            reinterpret_cast<sockaddr_in*>(current->ifa_addr);

        if (ntohl(ipv4->sin_addr.s_addr) == INADDR_LOOPBACK ||
            (ntohl(ipv4->sin_addr.s_addr) >> 24) == 169)
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
