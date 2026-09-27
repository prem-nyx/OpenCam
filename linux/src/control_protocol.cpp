#include "control_protocol.hpp"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <array>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>

namespace ControlProtocol
{
    namespace
    {
        constexpr std::array<std::uint8_t, 4> MAGIC{'O', 'C', 'A', 'M'};
        constexpr std::size_t HEADER_SIZE = 10;
        constexpr std::size_t AUTH_OVERHEAD = 8 + 32;

        bool waitFor(int fd, short events, int timeoutMs,
                     std::chrono::steady_clock::time_point deadline)
        {
            while (true)
            {
                int remaining = timeoutMs;
                if (timeoutMs >= 0)
                {
                    const auto now = std::chrono::steady_clock::now();
                    if (now >= deadline) return false;
                    remaining = static_cast<int>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            deadline - now).count());
                    if (remaining < 1) remaining = 1;
                }

                pollfd descriptor{fd, events, 0};
                const int result = poll(&descriptor, 1, remaining);
                if (result > 0)
                    return (descriptor.revents & (events | POLLERR | POLLHUP)) != 0;
                if (result == 0) return false;
                if (errno != EINTR) return false;
            }
        }

        bool transfer(int fd, std::uint8_t* data, std::size_t size,
                      bool writing, int timeoutMs,
                      std::chrono::steady_clock::time_point deadline)
        {
            std::size_t offset = 0;
            while (offset < size)
            {
                if (!waitFor(fd, writing ? POLLOUT : POLLIN, timeoutMs, deadline))
                    return false;
                const ssize_t count = writing
                    ? send(fd, data + offset, size - offset, MSG_NOSIGNAL)
                    : recv(fd, data + offset, size - offset, 0);
                if (count == 0) return false;
                if (count < 0)
                {
                    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                        continue;
                    return false;
                }
                offset += static_cast<std::size_t>(count);
            }
            return true;
        }

        bool hmac(const std::uint8_t* key, std::size_t keySize,
                  const std::uint8_t* input, std::size_t inputSize,
                  std::array<std::uint8_t, 32>& output)
        {
            unsigned int outputSize = 0;
            return HMAC(EVP_sha256(), key, static_cast<int>(keySize), input,
                        inputSize, output.data(), &outputSize) != nullptr &&
                   outputSize == output.size();
        }

        std::array<std::uint8_t, 8> sequenceBytes(std::uint64_t sequence)
        {
            std::array<std::uint8_t, 8> result{};
            for (int i = 7; i >= 0; --i)
            {
                result[static_cast<std::size_t>(i)] =
                    static_cast<std::uint8_t>(sequence & 0xff);
                sequence >>= 8;
            }
            return result;
        }

        std::array<std::uint8_t, 32> frameMac(
            const std::array<std::uint8_t, 32>& key, Type type,
            const std::array<std::uint8_t, 8>& sequence,
            const std::uint8_t* payload, std::size_t payloadSize)
        {
            std::vector<std::uint8_t> input;
            input.reserve(1 + sequence.size() + payloadSize);
            input.push_back(static_cast<std::uint8_t>(type));
            input.insert(input.end(), sequence.begin(), sequence.end());
            if (payloadSize != 0)
                input.insert(input.end(), payload, payload + payloadSize);
            std::array<std::uint8_t, 32> result{};
            if (!hmac(key.data(), key.size(), input.data(), input.size(), result))
                result.fill(0);
            return result;
        }

        std::chrono::steady_clock::time_point deadlineFor(int timeoutMs)
        {
            return timeoutMs < 0
                ? std::chrono::steady_clock::time_point::max()
                : std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        }
    }

    bool randomSecret(std::array<std::uint8_t, SECRET_SIZE>& secret)
    {
        return RAND_bytes(secret.data(), static_cast<int>(secret.size())) == 1;
    }

    std::string encodeHex(const std::uint8_t* data, std::size_t size)
    {
        static constexpr char digits[] = "0123456789abcdef";
        std::string result;
        result.reserve(size * 2);
        for (std::size_t i = 0; i < size; ++i)
        {
            result.push_back(digits[data[i] >> 4]);
            result.push_back(digits[data[i] & 0x0f]);
        }
        return result;
    }

    bool decodeHex(const std::string& text, std::uint8_t* output,
                   std::size_t size)
    {
        if (text.size() != size * 2) return false;
        auto nibble = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        for (std::size_t i = 0; i < size; ++i)
        {
            const int high = nibble(text[i * 2]);
            const int low = nibble(text[i * 2 + 1]);
            if (high < 0 || low < 0) return false;
            output[i] = static_cast<std::uint8_t>((high << 4) | low);
        }
        return true;
    }

    bool sendFrame(int fd, Type type, const std::vector<std::uint8_t>& payload,
                   int timeoutMs)
    {
        if (payload.size() > MAX_PAYLOAD) return false;
        std::array<std::uint8_t, HEADER_SIZE> header{};
        std::copy(MAGIC.begin(), MAGIC.end(), header.begin());
        header[4] = VERSION;
        header[5] = static_cast<std::uint8_t>(type);
        const auto size = static_cast<std::uint32_t>(payload.size());
        header[6] = static_cast<std::uint8_t>(size >> 24);
        header[7] = static_cast<std::uint8_t>(size >> 16);
        header[8] = static_cast<std::uint8_t>(size >> 8);
        header[9] = static_cast<std::uint8_t>(size);
        const auto deadline = deadlineFor(timeoutMs);
        return transfer(fd, header.data(), header.size(), true, timeoutMs, deadline) &&
               (payload.empty() || transfer(fd,
                   const_cast<std::uint8_t*>(payload.data()), payload.size(),
                   true, timeoutMs, deadline));
    }

    bool receiveFrame(int fd, Frame& frame, int timeoutMs)
    {
        std::array<std::uint8_t, HEADER_SIZE> header{};
        const auto deadline = deadlineFor(timeoutMs);
        if (!transfer(fd, header.data(), header.size(), false, timeoutMs, deadline))
            return false;
        if (!std::equal(MAGIC.begin(), MAGIC.end(), header.begin()) ||
            header[4] != VERSION)
            return false;
        const std::uint32_t size =
            (static_cast<std::uint32_t>(header[6]) << 24) |
            (static_cast<std::uint32_t>(header[7]) << 16) |
            (static_cast<std::uint32_t>(header[8]) << 8) |
            static_cast<std::uint32_t>(header[9]);
        if (size > MAX_PAYLOAD) return false;
        frame.type = static_cast<Type>(header[5]);
        frame.payload.resize(size);
        return size == 0 || transfer(fd, frame.payload.data(), size, false, timeoutMs, deadline);
    }

    bool deriveSessionKey(const std::array<std::uint8_t, SECRET_SIZE>& secret,
                          const std::array<std::uint8_t, NONCE_SIZE>& serverNonce,
                          const std::array<std::uint8_t, NONCE_SIZE>& clientNonce,
                          std::array<std::uint8_t, 32>& key)
    {
        static constexpr char domain[] = "OpenCam session v1";
        std::vector<std::uint8_t> input(domain, domain + sizeof(domain) - 1);
        input.insert(input.end(), serverNonce.begin(), serverNonce.end());
        input.insert(input.end(), clientNonce.begin(), clientNonce.end());
        return hmac(secret.data(), secret.size(), input.data(), input.size(), key);
    }

    std::string deriveMediaPassword(const std::array<std::uint8_t, 32>& sessionKey)
    {
        static constexpr char domain[] = "OpenCam RTSP credential v1";
        std::array<std::uint8_t, 32> credential{};
        if (!hmac(sessionKey.data(), sessionKey.size(),
                  reinterpret_cast<const std::uint8_t*>(domain), sizeof(domain) - 1,
                  credential)) return {};
        const std::string result = encodeHex(credential.data(), credential.size());
        OPENSSL_cleanse(credential.data(), credential.size());
        return result;
    }

    bool makeProof(const std::array<std::uint8_t, SECRET_SIZE>& secret,
                   const std::string& domain,
                   const std::array<std::uint8_t, NONCE_SIZE>& serverNonce,
                   const std::array<std::uint8_t, NONCE_SIZE>& clientNonce,
                   std::array<std::uint8_t, 32>& proof)
    {
        std::vector<std::uint8_t> input(domain.begin(), domain.end());
        input.insert(input.end(), serverNonce.begin(), serverNonce.end());
        input.insert(input.end(), clientNonce.begin(), clientNonce.end());
        return hmac(secret.data(), secret.size(), input.data(), input.size(), proof);
    }

    bool constantTimeEqual(const std::uint8_t* left, const std::uint8_t* right,
                           std::size_t size)
    {
        return CRYPTO_memcmp(left, right, size) == 0;
    }

    bool AuthenticatedChannel::send(Type type,
        const std::vector<std::uint8_t>& payload, int timeoutMs)
    {
        if (payload.size() + AUTH_OVERHEAD > MAX_PAYLOAD) return false;
        const auto sequence = sequenceBytes(sendSequence);
        const auto mac = frameMac(key, type, sequence, payload.data(), payload.size());
        std::vector<std::uint8_t> authenticated;
        authenticated.reserve(AUTH_OVERHEAD + payload.size());
        authenticated.insert(authenticated.end(), sequence.begin(), sequence.end());
        authenticated.insert(authenticated.end(), payload.begin(), payload.end());
        authenticated.insert(authenticated.end(), mac.begin(), mac.end());
        if (!sendFrame(fd, type, authenticated, timeoutMs)) return false;
        ++sendSequence;
        return true;
    }

    bool AuthenticatedChannel::receive(Frame& frame, int timeoutMs)
    {
        Frame wire{};
        if (!receiveFrame(fd, wire, timeoutMs) || wire.payload.size() < AUTH_OVERHEAD)
            return false;
        std::array<std::uint8_t, 8> sequence{};
        std::copy_n(wire.payload.begin(), sequence.size(), sequence.begin());
        std::uint64_t receivedSequence = 0;
        for (const auto byte : sequence) receivedSequence = (receivedSequence << 8) | byte;
        if (receivedSequence != receiveSequence) return false;
        const std::size_t contentSize = wire.payload.size() - AUTH_OVERHEAD;
        const auto expected = frameMac(key, wire.type, sequence,
            wire.payload.data() + sequence.size(), contentSize);
        const auto* receivedMac = wire.payload.data() + sequence.size() + contentSize;
        if (!constantTimeEqual(expected.data(), receivedMac, expected.size()))
            return false;
        frame.type = wire.type;
        frame.payload.assign(wire.payload.begin() + sequence.size(),
                             wire.payload.begin() + sequence.size() + contentSize);
        ++receiveSequence;
        return true;
    }
}
