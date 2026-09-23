#include "device_descriptor.hpp"
#include "stream_options.hpp"
#include "ffmpeg_runner.hpp"
#include "rtsp_probe.hpp"
#include "v4l2_device.hpp"
#include "network.hpp"
#include "qr.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <string>
#include <vector>

namespace
{
    constexpr int SERVER_PORT = 6969;
    constexpr int BACKLOG = 4;
    constexpr std::size_t RECEIVE_BUFFER_SIZE = 512;

    bool handleClient(
        int clientSocket,
        const std::string &clientIp)
    {
        std::cout << '\n';
        std::cout << "Android connected from "
                  << clientIp
                  << '\n';

        std::vector<std::uint8_t> descriptorData;
        descriptorData.reserve(1024);

        bool descriptorParsed = false;
        DeviceDescriptor descriptor{};

        while (!descriptorParsed)
        {
            std::uint8_t receiveBuffer[RECEIVE_BUFFER_SIZE];

            const ssize_t bytesReceived =
                recv(
                    clientSocket,
                    receiveBuffer,
                    sizeof(receiveBuffer),
                    0);

            if (bytesReceived < 0)
            {
                std::cerr
                    << "Failed to receive data: "
                    << std::strerror(errno)
                    << '\n';

                return false;
            }

            if (bytesReceived == 0)
            {
                std::cout
                    << "Android disconnected before "
                       "DeviceDescriptor was received.\n";

                return true;
            }

            descriptorData.insert(
                descriptorData.end(),
                receiveBuffer,
                receiveBuffer + bytesReceived);

            std::cout
                << "Received "
                << bytesReceived
                << " bytes ("
                << descriptorData.size()
                << " total)\n";

            try
            {
                descriptor =
                    parseDeviceDescriptor(descriptorData);

                descriptorParsed = true;
            }
            catch (const std::exception &)
            {
                // The descriptor may simply be incomplete.
            }
        }

        std::cout << '\n';
        std::cout << "Device detected\n";
        std::cout << "----------------\n";
        std::cout
            << "Name: "
            << descriptor.name
            << '\n';

        std::cout
            << "RTSP: "
            << descriptor.rtspUrl
            << '\n';

        std::cout
            << "Front resolutions: "
            << descriptor.frontResolutions.size()
            << '\n';

        std::cout
            << "Back resolutions: "
            << descriptor.backResolutions.size()
            << '\n';

        std::cout
            << "Filters: "
            << descriptor.filters.size()
            << '\n';

        /*
         * Make sure the virtual camera exists before
         * telling Android to start streaming.
         */
        std::string virtualCamera =
            findOpenCamDevice();

        if (virtualCamera.empty())
        {
            std::cout
                << "OpenCam virtual camera not found. "
                   "Loading v4l2loopback...\n";

            if (!loadV4L2Loopback())
            {
                std::cerr
                    << "Failed to load v4l2loopback.\n"
                    << "Please load the module manually:\n"
                    << "  sudo modprobe v4l2loopback "
                       "devices=1 card_label=OpenCam "
                       "exclusive_caps=1\n";

                return false;
            }

            virtualCamera =
                findOpenCamDevice();
        }

        if (virtualCamera.empty())
        {
            std::cerr
                << "OpenCam virtual camera could not "
                   "be found.\n";

            return false;
        }

        std::cout
            << "Virtual camera: "
            << virtualCamera
            << '\n';

        /*
         * Only activate Android after Linux has confirmed
         * that the video output device is available.
         */
        StreamOptions options;

        const std::vector<std::uint8_t>
            activationPacket =
                buildActivationPacket(options);

        std::cout
            << "Activation packet size: "
            << activationPacket.size()
            << " bytes\n";

        const ssize_t bytesSent =
            send(
                clientSocket,
                activationPacket.data(),
                activationPacket.size(),
                MSG_NOSIGNAL);

        if (bytesSent < 0)
        {
            std::cerr
                << "Failed to send activation packet: "
                << std::strerror(errno)
                << '\n';

            return false;
        }

        if (static_cast<std::size_t>(bytesSent) !=
            activationPacket.size())
        {
            std::cerr
                << "Activation packet was only partially "
                   "sent: "
                << bytesSent
                << " of "
                << activationPacket.size()
                << " bytes\n";

            return false;
        }

        std::cout
            << "Activation packet sent successfully.\n";

        std::cout
            << "Waiting for Android RTSP server...\n";

        if (!waitForRtsp(descriptor.rtspUrl))
        {
            return false;
        }

        const pid_t ffmpegPid =
            startFfmpeg(
                descriptor.rtspUrl,
                virtualCamera);

        if (ffmpegPid < 0)
        {
            return false;
        }

        std::cout
            << "FFmpeg started (PID: "
            << ffmpegPid
            << ").\n";

        std::cout
            << "Video bridge active.\n";

        std::cout
            << "Control connection remains open.\n";

        char buffer[1];

        while (true)
        {
            const ssize_t bytesRead =
                recv(
                    clientSocket,
                    buffer,
                    sizeof(buffer),
                    0);

            if (bytesRead == 0)
            {
                std::cout
                    << "Android disconnected.\n";

                break;
            }

            if (bytesRead < 0)
            {
                std::cerr
                    << "Control connection interrupted. "
                       "Cleaning up...\n";

                break;
            }
        }

        std::cout
            << "Stopping FFmpeg...\n";

        if (stopFfmpeg(ffmpegPid))
        {
            std::cout
                << "FFmpeg stopped successfully.\n";
        }
        else
        {
            std::cerr
                << "Failed to stop FFmpeg cleanly.\n";
        }

        return true;
    }
}

int main()
{
    std::cout
        << "OpenCam Linux controller\n";

    const std::string localIp =
        getLocalIpAddress();

    if (localIp.empty())
    {
        std::cerr
            << "Could not determine local IPv4 address.\n";

        return 1;
    }

    const std::string pairingPayload =
        localIp + ":" +
        std::to_string(SERVER_PORT);

    std::cout << '\n';
    std::cout
        << "OpenCam pairing\n";
    std::cout
        << "---------------\n";
    std::cout
        << "Address: "
        << pairingPayload
        << "\n\n";

    if (!displayPairingQr(pairingPayload))
    {
        std::cerr
            << "Failed to generate pairing QR.\n"
            << "Make sure qrencode is installed.\n";

        return 1;
    }

    std::cout << '\n';
    std::cout
        << "Scan the QR code with VCamdroid.\n\n";

    const int serverSocket =
        socket(
            AF_INET,
            SOCK_STREAM,
            0);

    if (serverSocket < 0)
    {
        std::cerr
            << "Failed to create socket: "
            << std::strerror(errno)
            << '\n';

        return 1;
    }

    int reuse = 1;

    if (setsockopt(
            serverSocket,
            SOL_SOCKET,
            SO_REUSEADDR,
            &reuse,
            sizeof(reuse)) < 0)
    {
        std::cerr
            << "Failed to configure socket: "
            << std::strerror(errno)
            << '\n';

        close(serverSocket);
        return 1;
    }

    sockaddr_in serverAddress{};

    serverAddress.sin_family =
        AF_INET;

    serverAddress.sin_addr.s_addr =
        htonl(INADDR_ANY);

    serverAddress.sin_port =
        htons(SERVER_PORT);

    if (bind(
            serverSocket,
            reinterpret_cast<sockaddr *>(
                &serverAddress),
            sizeof(serverAddress)) < 0)
    {
        std::cerr
            << "Failed to bind port "
            << SERVER_PORT
            << ": "
            << std::strerror(errno)
            << '\n';

        close(serverSocket);
        return 1;
    }

    if (listen(
            serverSocket,
            BACKLOG) < 0)
    {
        std::cerr
            << "Failed to listen: "
            << std::strerror(errno)
            << '\n';

        close(serverSocket);
        return 1;
    }

    std::cout
        << "Listening on TCP port "
        << SERVER_PORT
        << "...\n";

    while (true)
    {
        sockaddr_in clientAddress{};
        socklen_t clientAddressLength =
            sizeof(clientAddress);

        const int clientSocket =
            accept(
                serverSocket,
                reinterpret_cast<sockaddr *>(
                    &clientAddress),
                &clientAddressLength);

        if (clientSocket < 0)
        {
            std::cerr
                << "Failed to accept connection: "
                << std::strerror(errno)
                << '\n';

            continue;
        }

        char clientIp[INET_ADDRSTRLEN]{};

        if (inet_ntop(
                AF_INET,
                &clientAddress.sin_addr,
                clientIp,
                sizeof(clientIp)) == nullptr)
        {
            std::strncpy(
                clientIp,
                "unknown",
                sizeof(clientIp) - 1);
        }

        handleClient(
            clientSocket,
            clientIp);

        close(clientSocket);

        std::cout << '\n';
        std::cout
            << "Ready for another Android connection.\n";
    }

    close(serverSocket);

    return 0;
}