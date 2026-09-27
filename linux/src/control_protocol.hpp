#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace ControlProtocol
{
    constexpr std::uint8_t VERSION = 1;
    constexpr std::uint32_t MAX_PAYLOAD = 64 * 1024;
    constexpr std::size_t SECRET_SIZE = 32;
    constexpr std::size_t NONCE_SIZE = 32;

    enum class Type : std::uint8_t
    {
        SERVER_HELLO = 1,
        CLIENT_AUTH = 2,
        SERVER_AUTH = 3,
        DESCRIPTOR = 4,
        ACTIVATION = 5,
        ERROR_REPORT = 6
    };

    struct Frame
    {
        Type type;
        std::vector<std::uint8_t> payload;
    };

    struct AuthenticatedChannel
    {
        int fd;
        std::array<std::uint8_t, 32> key;
        std::uint64_t sendSequence = 0;
        std::uint64_t receiveSequence = 0;

        bool send(Type type, const std::vector<std::uint8_t>& payload,
                  int timeoutMs);
        bool receive(Frame& frame, int timeoutMs);
    };

    bool randomSecret(std::array<std::uint8_t, SECRET_SIZE>& secret);
    std::string encodeHex(const std::uint8_t* data, std::size_t size);
    bool decodeHex(const std::string& text, std::uint8_t* output,
                   std::size_t size);

    bool sendFrame(int fd, Type type, const std::vector<std::uint8_t>& payload,
                   int timeoutMs);
    bool receiveFrame(int fd, Frame& frame, int timeoutMs);
    bool deriveSessionKey(const std::array<std::uint8_t, SECRET_SIZE>& secret,
                          const std::array<std::uint8_t, NONCE_SIZE>& serverNonce,
                          const std::array<std::uint8_t, NONCE_SIZE>& clientNonce,
                          std::array<std::uint8_t, 32>& key);
    std::string deriveMediaPassword(const std::array<std::uint8_t, 32>& sessionKey);
    bool makeProof(const std::array<std::uint8_t, SECRET_SIZE>& secret,
                   const std::string& domain,
                   const std::array<std::uint8_t, NONCE_SIZE>& serverNonce,
                   const std::array<std::uint8_t, NONCE_SIZE>& clientNonce,
                   std::array<std::uint8_t, 32>& proof);
    bool constantTimeEqual(const std::uint8_t* left, const std::uint8_t* right,
                           std::size_t size);
}
