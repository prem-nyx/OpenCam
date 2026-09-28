#include "control_protocol.hpp"
#include "device_descriptor.hpp"
#include "security_validation.hpp"

#include <array>
#include <cassert>
#include <stdexcept>
#include <vector>

#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using namespace ControlProtocol;

int main()
{
    std::array<std::uint8_t, SECRET_SIZE> secret{};
    std::array<std::uint8_t, NONCE_SIZE> serverNonce{};
    std::array<std::uint8_t, NONCE_SIZE> clientNonce{};
    secret.fill(0x41);
    serverNonce.fill(0x12);
    clientNonce.fill(0x34);

    std::array<std::uint8_t, 32> proof{};
    assert(makeProof(secret, "OpenCam client proof v1", serverNonce,
                     clientNonce, proof));
    auto wrongSecret = secret;
    wrongSecret[0] ^= 1;
    std::array<std::uint8_t, 32> wrongProof{};
    assert(makeProof(wrongSecret, "OpenCam client proof v1", serverNonce,
                     clientNonce, wrongProof));
    assert(!constantTimeEqual(proof.data(), wrongProof.data(), proof.size()));

    std::array<std::uint8_t, 32> key{};
    assert(deriveSessionKey(secret, serverNonce, clientNonce, key));
    std::array<std::uint8_t, 32> zeroSessionKey{};
    assert(deriveMediaPassword(zeroSessionKey) ==
       "3d71315941fed222d06ace2d99f2f5210fdbe4df5c3be2dc673e13c414bac9b5");
    int sockets[2]{};
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    AuthenticatedChannel sender{sockets[0], key, true};
    AuthenticatedChannel receiver{sockets[1], key, false};
    const std::vector<std::uint8_t> message{'f', 'r', 'a', 'm', 'e'};
    bool sent = false;
    std::thread writer([&]
                       { sent = sender.send(Type::DESCRIPTOR, message, 3000); });
    Frame received{};
    const bool receivedOk = receiver.receive(received, 3000);
    writer.join();
    assert(sent && receivedOk);
    assert(received.type == Type::DESCRIPTOR && received.payload == message);
    close(sockets[0]);
    close(sockets[1]);

    std::vector<std::uint8_t> invalidUtf8{
        0, 1, 0xff, // name
        0, 1, 'x',  // URL
        0, 0,       // front resolutions
        0, 0,       // back resolutions
        0, 0        // filters
    };
    bool rejected = false;
    try
    {
        (void)parseDeviceDescriptor(invalidUtf8);
    }
    catch (const std::exception &)
    {
        rejected = true;
    }
    assert(rejected);

    std::vector<std::uint8_t> hugeCount{
        0, 1, 'n', 0, 1, 'u',
        0xff, 0xff};
    rejected = false;
    try
    {
        (void)parseDeviceDescriptor(hugeCount);
    }
    catch (const std::exception &)
    {
        rejected = true;
    }
    assert(rejected);

    assert(isAllowedRtspEndpoint("rtsp://10.0.0.2:8554/live/0123456789abcdef0123456789abcdef", "10.0.0.2"));
    assert(!isAllowedRtspEndpoint("file:///dev/null", "10.0.0.2"));
    assert(!isAllowedRtspEndpoint("rtsp://10.0.0.3:8554/live/0123456789abcdef0123456789abcdef", "10.0.0.2"));
    assert(!isAllowedRtspEndpoint("rtsp://10.0.0.2:554/live/0123456789abcdef0123456789abcdef", "10.0.0.2"));
    assert(!isAllowedRtspEndpoint("rtsp://user:pass@10.0.0.2:8554/live/0123456789abcdef0123456789abcdef", "10.0.0.2"));
    assert(!isAllowedRtspEndpoint("rtsp://10.0.0.2:8554/live/0123456789abcdef0123456789abcdef?x=1", "10.0.0.2"));
    return 0;
}
