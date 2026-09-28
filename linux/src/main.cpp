#include "control_protocol.hpp"
#include "device_descriptor.hpp"
#include "ffmpeg_runner.hpp"
#include "network.hpp"
#include "qr.hpp"
#include "rtsp_probe.hpp"
#include "security_validation.hpp"
#include "stream_options.hpp"
#include "v4l2_device.hpp"

#include <arpa/inet.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <openssl/crypto.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace
{
    constexpr int SERVER_PORT = 6969;
    constexpr int BACKLOG = 4;
    constexpr int HANDSHAKE_TIMEOUT_MS = 5000;
    constexpr int DESCRIPTOR_TIMEOUT_MS = 30000;
    constexpr auto PAIRING_LIFETIME = std::chrono::minutes(10);

    bool handleClient(
        int clientSocket,
        const std::string &clientIp,
        const std::array<std::uint8_t, ControlProtocol::SECRET_SIZE> &secret,
        std::chrono::steady_clock::time_point pairingExpires,
        bool &authenticated)
    {
        using namespace ControlProtocol;

        authenticated = false;

        std::array<std::uint8_t, NONCE_SIZE> serverNonce{};
        if (!randomSecret(serverNonce))
            return false;

        std::cout
            << "Control connection from "
            << clientIp
            << ".\n";

        if (!sendFrame(
                clientSocket,
                Type::SERVER_HELLO,
                std::vector<std::uint8_t>(
                    serverNonce.begin(),
                    serverNonce.end()),
                HANDSHAKE_TIMEOUT_MS))
        {
            return false;
        }

        Frame authFrame{};

        if (!receiveFrame(
                clientSocket,
                authFrame,
                HANDSHAKE_TIMEOUT_MS) ||
            authFrame.type != Type::CLIENT_AUTH ||
            authFrame.payload.size() != 64)
        {
            return false;
        }

        std::array<std::uint8_t, NONCE_SIZE> clientNonce{};

        std::copy_n(
            authFrame.payload.begin(),
            NONCE_SIZE,
            clientNonce.begin());

        std::array<std::uint8_t, 32> expectedClientProof{};

        if (!makeProof(
                secret,
                "OpenCam client proof v2",
                serverNonce,
                clientNonce,
                expectedClientProof) ||
            !constantTimeEqual(
                expectedClientProof.data(),
                authFrame.payload.data() + NONCE_SIZE,
                expectedClientProof.size()))
        {
            std::cerr
                << "Pairing authentication rejected.\n";

            return false;
        }

        if (std::chrono::steady_clock::now() >= pairingExpires)
            return false;

        std::array<std::uint8_t, 32> serverProof{};
        std::array<std::uint8_t, 32> sessionKey{};

        if (!makeProof(
                secret,
                "OpenCam server proof v2",
                serverNonce,
                clientNonce,
                serverProof) ||
            !deriveSessionKey(
                secret,
                serverNonce,
                clientNonce,
                sessionKey) ||
            !sendFrame(
                clientSocket,
                Type::SERVER_AUTH,
                std::vector<std::uint8_t>(
                    serverProof.begin(),
                    serverProof.end()),
                HANDSHAKE_TIMEOUT_MS))
        {
            return false;
        }

        // QR secret is single-use after successful proof.
        authenticated = true;

        AuthenticatedChannel channel{
            clientSocket,
            sessionKey,
            true};

        Frame descriptorFrame{};

        if (!channel.receive(
                descriptorFrame,
                DESCRIPTOR_TIMEOUT_MS) ||
            descriptorFrame.type != Type::DESCRIPTOR)
        {
            std::cerr
                << "Authenticated descriptor frame missing, "
                   "invalid, or timed out.\n";

            return false;
        }

        DeviceDescriptor descriptor;

        try
        {
            descriptor =
                parseDeviceDescriptor(
                    descriptorFrame.payload);
        }
        catch (const std::exception &)
        {
            std::cerr
                << "Authenticated client sent an invalid descriptor.\n";

            return false;
        }

        if (!isAllowedRtspEndpoint(
                descriptor.rtspUrl,
                clientIp))
        {
            std::cerr
                << "Rejected descriptor with an invalid media endpoint.\n";

            return false;
        }

        std::cout
            << "Authenticated descriptor accepted; "
               "preparing activation.\n";

        const std::string mediaPassword =
            deriveMediaPassword(sessionKey);

        if (mediaPassword.size() != 64)
            return false;

        const std::string authenticatedRtspUrl =
            "rtsp://opencam:" +
            mediaPassword +
            "@" +
            descriptor.rtspUrl.substr(7);

        const std::string virtualCamera =
            findOpenCamDevice();

        if (virtualCamera.empty())
        {
            std::cerr
                << "OpenCam V4L2 device is unavailable; "
                   "load v4l2loopback first.\n";

            return false;
        }

        StreamOptions options;

        const auto activation =
            buildActivationPacket(options);

        if (!channel.send(
                Type::ACTIVATION,
                activation,
                HANDSHAKE_TIMEOUT_MS))
        {
            std::cerr
                << "Could not send activation to "
                   "authenticated client.\n";

            return false;
        }

        std::cout
            << "Activation sent; starting RTSP readiness check.\n";

        if (!waitForRtsp(authenticatedRtspUrl))
            return false;

        FfmpegProcess ffmpegProcess =
            startFfmpeg(
                authenticatedRtspUrl,
                virtualCamera);

        if (ffmpegProcess.pid < 0)
            return false;

        std::cout
            << "FFmpeg started (PID "
            << ffmpegProcess.pid
            << ").\n";

        bool childExited = false;
        bool connected = true;

        while (connected)
        {
            /*
             * Drain FFmpeg stderr first so its non-blocking
             * diagnostic pipe cannot fill and stall FFmpeg.
             */
            const std::string diagnostics =
                drainFfmpegStderr(ffmpegProcess);

            if (!diagnostics.empty())
            {
                std::cerr
                    << "FFmpeg diagnostics:\n"
                    << diagnostics;

                if (diagnostics.back() != '\n')
                    std::cerr << '\n';
            }

            int childStatus = 0;

            const pid_t childResult =
                waitpid(
                    ffmpegProcess.pid,
                    &childStatus,
                    WNOHANG);

            if (childResult == ffmpegProcess.pid ||
                (childResult < 0 && errno == ECHILD))
            {
                childExited = true;

                /*
                 * Drain any diagnostics written immediately
                 * before process termination.
                 */
                const std::string finalDiagnostics =
                    drainFfmpegStderr(ffmpegProcess);

                if (!finalDiagnostics.empty())
                {
                    std::cerr
                        << "FFmpeg diagnostics:\n"
                        << finalDiagnostics;

                    if (finalDiagnostics.back() != '\n')
                        std::cerr << '\n';
                }

                if (childResult == ffmpegProcess.pid &&
                    WIFEXITED(childStatus))
                {
                    std::cerr
                        << "FFmpeg exited with status "
                        << WEXITSTATUS(childStatus)
                        << "; ending authenticated session.\n";
                }
                else if (childResult == ffmpegProcess.pid &&
                         WIFSIGNALED(childStatus))
                {
                    std::cerr
                        << "FFmpeg terminated by signal "
                        << WTERMSIG(childStatus)
                        << "; ending authenticated session.\n";
                }
                else
                {
                    std::cerr
                        << "FFmpeg child status unavailable; "
                           "ending authenticated session.\n";
                }

                break;
            }

            pollfd descriptorPoll{
                clientSocket,
                POLLIN,
                0};

            const int ready =
                poll(
                    &descriptorPoll,
                    1,
                    250);

            if (ready < 0)
            {
                if (errno == EINTR)
                    continue;

                break;
            }

            if (ready == 0)
                continue;

            if (descriptorPoll.revents &
                (POLLERR | POLLHUP | POLLNVAL))
            {
                break;
            }

            Frame incoming{};

            if (!channel.receive(
                    incoming,
                    2000) ||
                incoming.type != Type::ERROR_REPORT)
            {
                std::cerr
                    << "Invalid authenticated control frame; "
                       "closing session.\n";

                break;
            }

            // Error text is peer-controlled.
            // Do not echo it into terminal logs.
            std::cerr
                << "Authenticated Android client "
                   "reported an error.\n";
        }

        if (!childExited &&
            !stopFfmpeg(ffmpegProcess))
        {
            std::cerr
                << "FFmpeg required forced cleanup.\n";
        }

        return true;
    }

    bool showPairingQr(
        const std::string &address,
        const std::array<
            std::uint8_t,
            ControlProtocol::SECRET_SIZE> &secret)
    {
        const std::string payload =
            "OCAM1|" +
            address +
            "|" +
            std::to_string(SERVER_PORT) +
            "|" +
            ControlProtocol::encodeHex(
                secret.data(),
                secret.size());

        std::cout
            << "\nOpenCam pairing token "
               "(one successful use):\n";

        if (!displayPairingQr(payload))
            return false;

        std::cout
            << "Scan this QR code with VCamdroid. "
               "The token is not logged.\n";

        return true;
    }
}

int main()
{
    using namespace ControlProtocol;

    std::cout
        << "OpenCam Linux controller "
           "(control protocol v1)\n";

    const std::string localIp =
        getLocalIpAddress();

    if (localIp.empty())
    {
        std::cerr
            << "Could not determine a route-selected "
               "local IPv4 address.\n";

        return 1;
    }

    const int serverSocket =
        socket(
            AF_INET,
            SOCK_STREAM | SOCK_CLOEXEC,
            0);

    if (serverSocket < 0)
    {
        std::cerr
            << "Failed to create control socket: "
            << std::strerror(errno)
            << '\n';

        return 1;
    }

    int reuse = 1;

    setsockopt(
        serverSocket,
        SOL_SOCKET,
        SO_REUSEADDR,
        &reuse,
        sizeof(reuse));

    sockaddr_in serverAddress{};

    serverAddress.sin_family =
        AF_INET;

    serverAddress.sin_port =
        htons(SERVER_PORT);

    if (inet_pton(
            AF_INET,
            localIp.c_str(),
            &serverAddress.sin_addr) != 1 ||
        bind(
            serverSocket,
            reinterpret_cast<sockaddr *>(
                &serverAddress),
            sizeof(serverAddress)) < 0 ||
        listen(
            serverSocket,
            BACKLOG) < 0)
    {
        std::cerr
            << "Could not bind control service to "
            << localIp
            << ':'
            << SERVER_PORT
            << ": "
            << std::strerror(errno)
            << '\n';

        close(serverSocket);
        return 1;
    }

    std::cout
        << "Control listener bound to "
        << localIp
        << ':'
        << SERVER_PORT
        << " (IPv4 only).\n";

    std::array<
        std::uint8_t,
        SECRET_SIZE>
        pairingSecret{};

    auto pairingExpires =
        std::chrono::steady_clock::now() +
        PAIRING_LIFETIME;

    while (randomSecret(pairingSecret))
    {
        if (!showPairingQr(
                localIp,
                pairingSecret))
        {
            std::cerr
                << "Could not display pairing QR; "
                   "ensure qrencode is installed.\n";

            close(serverSocket);
            return 1;
        }

        pollfd listener{
            serverSocket,
            POLLIN,
            0};

        const auto remaining =
            std::chrono::duration_cast<
                std::chrono::milliseconds>(
                pairingExpires -
                std::chrono::steady_clock::now())
                .count();

        const int ready =
            poll(
                &listener,
                1,
                static_cast<int>(
                    std::max<int64_t>(
                        0,
                        remaining)));

        if (ready == 0 ||
            std::chrono::steady_clock::now() >=
                pairingExpires)
        {
            OPENSSL_cleanse(
                pairingSecret.data(),
                pairingSecret.size());

            if (!randomSecret(pairingSecret))
                break;

            pairingExpires =
                std::chrono::steady_clock::now() +
                PAIRING_LIFETIME;

            continue;
        }

        if (ready < 0)
        {
            if (errno == EINTR)
                continue;

            std::cerr
                << "Control listener wait failed.\n";

            continue;
        }

        sockaddr_in clientAddress{};
        socklen_t clientAddressLength =
            sizeof(clientAddress);

        const int clientSocket =
            accept4(
                serverSocket,
                reinterpret_cast<sockaddr *>(
                    &clientAddress),
                &clientAddressLength,
                SOCK_CLOEXEC);

        if (clientSocket < 0)
        {
            if (errno == EINTR)
                continue;

            std::cerr
                << "Control accept failed: "
                << std::strerror(errno)
                << '\n';

            continue;
        }

        if (std::chrono::steady_clock::now() >=
            pairingExpires)
        {
            close(clientSocket);

            OPENSSL_cleanse(
                pairingSecret.data(),
                pairingSecret.size());

            if (!randomSecret(pairingSecret))
                break;

            pairingExpires =
                std::chrono::steady_clock::now() +
                PAIRING_LIFETIME;

            continue;
        }

        char clientIp[INET_ADDRSTRLEN]{};

        if (inet_ntop(
                AF_INET,
                &clientAddress.sin_addr,
                clientIp,
                sizeof(clientIp)) == nullptr)
        {
            std::strcpy(
                clientIp,
                "unknown");
        }

        bool authenticated = false;

        handleClient(
            clientSocket,
            clientIp,
            pairingSecret,
            pairingExpires,
            authenticated);

        close(clientSocket);

        if (authenticated)
        {
            /*
             * Replace the QR token after a successful
             * pairing; stale QR codes cannot open another
             * control session.
             */
            OPENSSL_cleanse(
                pairingSecret.data(),
                pairingSecret.size());

            if (!randomSecret(pairingSecret))
                break;

            pairingExpires =
                std::chrono::steady_clock::now() +
                PAIRING_LIFETIME;
        }
    }

    OPENSSL_cleanse(
        pairingSecret.data(),
        pairingSecret.size());

    close(serverSocket);

    return 1;
}