#include "control_protocol.hpp"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/kdf.h>
#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <poll.h>
#include <stdexcept>
#include <sys/socket.h>
#include <vector>

namespace ControlProtocol
{
    namespace
    {
        constexpr std::array<std::uint8_t, 4> MAGIC{
            'O', 'C', 'A', 'M'};

        constexpr std::size_t HEADER_SIZE = 10;

        constexpr std::array<std::uint8_t, 4> SERVER_NONCE_PREFIX{
            'S', 'R', 'V', 'R'};

        constexpr std::array<std::uint8_t, 4> CLIENT_NONCE_PREFIX{
            'C', 'L', 'N', 'T'};

        constexpr char SESSION_KDF_INFO[] =
            "OpenCam control v2 session";

        constexpr char SERVER_TO_CLIENT_KDF_INFO[] =
            "OpenCam control v2 server-to-client";

        constexpr char CLIENT_TO_SERVER_KDF_INFO[] =
            "OpenCam control v2 client-to-server";

        constexpr char MEDIA_CREDENTIAL_DOMAIN[] =
            "OpenCam RTSP credential v2";

        bool waitFor(
            int fd,
            short events,
            int timeoutMs,
            std::chrono::steady_clock::time_point deadline)
        {
            while (true)
            {
                int remaining = timeoutMs;

                if (timeoutMs >= 0)
                {
                    const auto now =
                        std::chrono::steady_clock::now();

                    if (now >= deadline)
                        return false;

                    remaining = static_cast<int>(
                        std::chrono::duration_cast<
                            std::chrono::milliseconds>(
                            deadline - now)
                            .count());

                    if (remaining < 1)
                        remaining = 1;
                }

                pollfd descriptor{
                    fd,
                    events,
                    0};

                const int result =
                    poll(&descriptor, 1, remaining);

                if (result > 0)
                {
                    return (
                               descriptor.revents &
                               (events | POLLERR | POLLHUP)) != 0;
                }

                if (result == 0)
                    return false;

                if (errno != EINTR)
                    return false;
            }
        }

        bool transfer(
            int fd,
            std::uint8_t *data,
            std::size_t size,
            bool writing,
            int timeoutMs,
            std::chrono::steady_clock::time_point deadline)
        {
            std::size_t offset = 0;

            while (offset < size)
            {
                if (!waitFor(
                        fd,
                        writing ? POLLOUT : POLLIN,
                        timeoutMs,
                        deadline))
                {
                    return false;
                }

                const ssize_t count =
                    writing
                        ? send(
                              fd,
                              data + offset,
                              size - offset,
                              MSG_NOSIGNAL)
                        : recv(
                              fd,
                              data + offset,
                              size - offset,
                              0);

                if (count == 0)
                    return false;

                if (count < 0)
                {
                    if (
                        errno == EINTR ||
                        errno == EAGAIN ||
                        errno == EWOULDBLOCK)
                    {
                        continue;
                    }

                    return false;
                }

                offset += static_cast<std::size_t>(count);
            }

            return true;
        }

        bool hmac(
            const std::uint8_t *key,
            std::size_t keySize,
            const std::uint8_t *input,
            std::size_t inputSize,
            std::array<std::uint8_t, 32> &output)
        {
            unsigned int outputSize = 0;

            return HMAC(
                       EVP_sha256(),
                       key,
                       static_cast<int>(keySize),
                       input,
                       inputSize,
                       output.data(),
                       &outputSize) != nullptr &&
                   outputSize == output.size();
        }

        std::array<std::uint8_t, 8> sequenceBytes(
            std::uint64_t sequence)
        {
            std::array<std::uint8_t, 8> result{};

            for (int i = 7; i >= 0; --i)
            {
                result[static_cast<std::size_t>(i)] =
                    static_cast<std::uint8_t>(
                        sequence & 0xff);

                sequence >>= 8;
            }

            return result;
        }

        std::array<std::uint8_t, HEADER_SIZE> makeHeader(
            Type type,
            std::size_t payloadSize)
        {
            std::array<std::uint8_t, HEADER_SIZE> header{};

            std::copy(
                MAGIC.begin(),
                MAGIC.end(),
                header.begin());

            header[4] = VERSION;
            header[5] =
                static_cast<std::uint8_t>(type);

            const auto size =
                static_cast<std::uint32_t>(payloadSize);

            header[6] =
                static_cast<std::uint8_t>(size >> 24);

            header[7] =
                static_cast<std::uint8_t>(size >> 16);

            header[8] =
                static_cast<std::uint8_t>(size >> 8);

            header[9] =
                static_cast<std::uint8_t>(size);

            return header;
        }

        std::array<std::uint8_t, GCM_NONCE_SIZE>
        makeGcmNonce(
            const std::array<std::uint8_t, 4> &prefix,
            const std::array<std::uint8_t, 8> &sequence)
        {
            std::array<std::uint8_t, GCM_NONCE_SIZE> nonce{};

            std::copy(
                prefix.begin(),
                prefix.end(),
                nonce.begin());

            std::copy(
                sequence.begin(),
                sequence.end(),
                nonce.begin() + prefix.size());

            return nonce;
        }

        bool hkdfExpand(
            const std::array<std::uint8_t, KEY_SIZE> &prk,
            const char *info,
            std::array<std::uint8_t, KEY_SIZE> &output)
        {
            EVP_PKEY_CTX *context =
                EVP_PKEY_CTX_new_id(
                    EVP_PKEY_HKDF,
                    nullptr);

            if (!context)
                return false;

            bool success = false;

            do
            {
                if (EVP_PKEY_derive_init(context) <= 0)
                    break;

                if (EVP_PKEY_CTX_set_hkdf_mode(
                        context,
                        EVP_PKEY_HKDEF_MODE_EXPAND_ONLY) <= 0)
                    break;

                if (EVP_PKEY_CTX_set_hkdf_md(
                        context,
                        EVP_sha256()) <= 0)
                    break;

                if (EVP_PKEY_CTX_set1_hkdf_key(
                        context,
                        prk.data(),
                        static_cast<int>(prk.size())) <= 0)
                    break;

                if (EVP_PKEY_CTX_add1_hkdf_info(
                        context,
                        reinterpret_cast<const unsigned char *>(info),
                        static_cast<int>(std::strlen(info))) <= 0)
                    break;

                std::size_t outputSize =
                    output.size();

                if (EVP_PKEY_derive(
                        context,
                        output.data(),
                        &outputSize) <= 0)
                    break;

                if (outputSize != output.size())
                    break;

                success = true;
            } while (false);

            EVP_PKEY_CTX_free(context);

            return success;
        }

        bool hkdfExtractAndExpand(
            const std::array<std::uint8_t, SECRET_SIZE> &secret,
            const std::array<std::uint8_t, NONCE_SIZE> &serverNonce,
            const std::array<std::uint8_t, NONCE_SIZE> &clientNonce,
            const char *info,
            std::array<std::uint8_t, KEY_SIZE> &output)
        {
            std::array<std::uint8_t, NONCE_SIZE * 2> salt{};

            std::copy(
                serverNonce.begin(),
                serverNonce.end(),
                salt.begin());

            std::copy(
                clientNonce.begin(),
                clientNonce.end(),
                salt.begin() + serverNonce.size());

            EVP_PKEY_CTX *context =
                EVP_PKEY_CTX_new_id(
                    EVP_PKEY_HKDF,
                    nullptr);

            if (!context)
                return false;

            bool success = false;

            do
            {
                if (EVP_PKEY_derive_init(context) <= 0)
                    break;

                if (EVP_PKEY_CTX_set_hkdf_mode(
                        context,
                        EVP_PKEY_HKDEF_MODE_EXTRACT_AND_EXPAND) <= 0)
                    break;

                if (EVP_PKEY_CTX_set_hkdf_md(
                        context,
                        EVP_sha256()) <= 0)
                    break;

                if (EVP_PKEY_CTX_set1_hkdf_salt(
                        context,
                        salt.data(),
                        static_cast<int>(salt.size())) <= 0)
                    break;

                if (EVP_PKEY_CTX_set1_hkdf_key(
                        context,
                        secret.data(),
                        static_cast<int>(secret.size())) <= 0)
                    break;

                if (EVP_PKEY_CTX_add1_hkdf_info(
                        context,
                        reinterpret_cast<const unsigned char *>(info),
                        static_cast<int>(std::strlen(info))) <= 0)
                    break;

                std::size_t outputSize =
                    output.size();

                if (EVP_PKEY_derive(
                        context,
                        output.data(),
                        &outputSize) <= 0)
                    break;

                if (outputSize != output.size())
                    break;

                success = true;
            } while (false);

            EVP_PKEY_CTX_free(context);

            OPENSSL_cleanse(
                salt.data(),
                salt.size());

            return success;
        }

        bool encryptGcm(
            const std::array<std::uint8_t, KEY_SIZE> &key,
            const std::array<std::uint8_t, GCM_NONCE_SIZE> &nonce,
            const std::uint8_t *aad,
            std::size_t aadSize,
            const std::vector<std::uint8_t> &plaintext,
            std::vector<std::uint8_t> &ciphertext)
        {
            EVP_CIPHER_CTX *context =
                EVP_CIPHER_CTX_new();

            if (!context)
                return false;

            bool success = false;

            do
            {
                if (EVP_EncryptInit_ex(
                        context,
                        EVP_aes_256_gcm(),
                        nullptr,
                        nullptr,
                        nullptr) <= 0)
                    break;

                if (EVP_CIPHER_CTX_ctrl(
                        context,
                        EVP_CTRL_AEAD_SET_IVLEN,
                        GCM_NONCE_SIZE,
                        nullptr) <= 0)
                    break;

                if (EVP_EncryptInit_ex(
                        context,
                        nullptr,
                        nullptr,
                        key.data(),
                        nonce.data()) <= 0)
                    break;

                int ignored = 0;

                if (EVP_EncryptUpdate(
                        context,
                        nullptr,
                        &ignored,
                        aad,
                        static_cast<int>(aadSize)) <= 0)
                    break;

                ciphertext.resize(
                    plaintext.size() + GCM_TAG_SIZE);

                int ciphertextSize = 0;

                if (!plaintext.empty())
                {
                    if (EVP_EncryptUpdate(
                            context,
                            ciphertext.data(),
                            &ciphertextSize,
                            plaintext.data(),
                            static_cast<int>(plaintext.size())) <= 0)
                        break;
                }

                int finalSize = 0;

                if (EVP_EncryptFinal_ex(
                        context,
                        ciphertext.data() + ciphertextSize,
                        &finalSize) <= 0)
                    break;

                ciphertextSize += finalSize;

                if (EVP_CIPHER_CTX_ctrl(
                        context,
                        EVP_CTRL_AEAD_GET_TAG,
                        GCM_TAG_SIZE,
                        ciphertext.data() + ciphertextSize) <= 0)
                    break;

                ciphertext.resize(
                    static_cast<std::size_t>(
                        ciphertextSize + GCM_TAG_SIZE));

                success = true;
            } while (false);

            EVP_CIPHER_CTX_free(context);

            return success;
        }

        bool decryptGcm(
            const std::array<std::uint8_t, KEY_SIZE> &key,
            const std::array<std::uint8_t, GCM_NONCE_SIZE> &nonce,
            const std::uint8_t *aad,
            std::size_t aadSize,
            const std::uint8_t *ciphertext,
            std::size_t ciphertextSize,
            const std::uint8_t *tag,
            std::vector<std::uint8_t> &plaintext)
        {
            EVP_CIPHER_CTX *context =
                EVP_CIPHER_CTX_new();

            if (!context)
                return false;

            bool success = false;

            do
            {
                if (EVP_DecryptInit_ex(
                        context,
                        EVP_aes_256_gcm(),
                        nullptr,
                        nullptr,
                        nullptr) <= 0)
                    break;

                if (EVP_CIPHER_CTX_ctrl(
                        context,
                        EVP_CTRL_AEAD_SET_IVLEN,
                        GCM_NONCE_SIZE,
                        nullptr) <= 0)
                    break;

                if (EVP_DecryptInit_ex(
                        context,
                        nullptr,
                        nullptr,
                        key.data(),
                        nonce.data()) <= 0)
                    break;

                int ignored = 0;

                if (EVP_DecryptUpdate(
                        context,
                        nullptr,
                        &ignored,
                        aad,
                        static_cast<int>(aadSize)) <= 0)
                    break;

                plaintext.resize(ciphertextSize);

                int plaintextSize = 0;

                if (ciphertextSize != 0)
                {
                    if (EVP_DecryptUpdate(
                            context,
                            plaintext.data(),
                            &plaintextSize,
                            ciphertext,
                            static_cast<int>(ciphertextSize)) <= 0)
                        break;
                }

                if (EVP_CIPHER_CTX_ctrl(
                        context,
                        EVP_CTRL_AEAD_SET_TAG,
                        GCM_TAG_SIZE,
                        const_cast<std::uint8_t *>(tag)) <= 0)
                    break;

                int finalSize = 0;

                if (EVP_DecryptFinal_ex(
                        context,
                        plaintext.data() + plaintextSize,
                        &finalSize) <= 0)
                    break;

                plaintext.resize(
                    static_cast<std::size_t>(
                        plaintextSize + finalSize));

                success = true;
            } while (false);

            if (!success)
                plaintext.clear();

            EVP_CIPHER_CTX_free(context);

            return success;
        }

        std::chrono::steady_clock::time_point deadlineFor(
            int timeoutMs)
        {
            return timeoutMs < 0
                       ? std::chrono::steady_clock::time_point::max()
                       : std::chrono::steady_clock::now() +
                             std::chrono::milliseconds(timeoutMs);
        }
    }

    bool randomSecret(
        std::array<std::uint8_t, SECRET_SIZE> &secret)
    {
        return RAND_bytes(
                   secret.data(),
                   static_cast<int>(secret.size())) == 1;
    }

    std::string encodeHex(
        const std::uint8_t *data,
        std::size_t size)
    {
        static constexpr char digits[] =
            "0123456789abcdef";

        std::string result;
        result.reserve(size * 2);

        for (std::size_t i = 0; i < size; ++i)
        {
            result.push_back(
                digits[data[i] >> 4]);

            result.push_back(
                digits[data[i] & 0x0f]);
        }

        return result;
    }

    bool decodeHex(
        const std::string &text,
        std::uint8_t *output,
        std::size_t size)
    {
        if (text.size() != size * 2)
            return false;

        auto nibble = [](char c) -> int
        {
            if (c >= '0' && c <= '9')
                return c - '0';

            if (c >= 'a' && c <= 'f')
                return c - 'a' + 10;

            if (c >= 'A' && c <= 'F')
                return c - 'A' + 10;

            return -1;
        };

        for (std::size_t i = 0; i < size; ++i)
        {
            const int high =
                nibble(text[i * 2]);

            const int low =
                nibble(text[i * 2 + 1]);

            if (high < 0 || low < 0)
                return false;

            output[i] =
                static_cast<std::uint8_t>(
                    (high << 4) | low);
        }

        return true;
    }

    bool sendFrame(
        int fd,
        Type type,
        const std::vector<std::uint8_t> &payload,
        int timeoutMs)
    {
        if (payload.size() > MAX_PAYLOAD)
            return false;

        const auto header =
            makeHeader(type, payload.size());

        const auto deadline =
            deadlineFor(timeoutMs);

        return transfer(
                   fd,
                   const_cast<std::uint8_t *>(
                       header.data()),
                   header.size(),
                   true,
                   timeoutMs,
                   deadline) &&
               (payload.empty() ||
                transfer(
                    fd,
                    const_cast<std::uint8_t *>(
                        payload.data()),
                    payload.size(),
                    true,
                    timeoutMs,
                    deadline));
    }

    bool receiveFrame(
        int fd,
        Frame &frame,
        int timeoutMs)
    {
        std::array<std::uint8_t, HEADER_SIZE> header{};

        const auto deadline =
            deadlineFor(timeoutMs);

        if (!transfer(
                fd,
                header.data(),
                header.size(),
                false,
                timeoutMs,
                deadline))
        {
            return false;
        }

        if (
            !std::equal(
                MAGIC.begin(),
                MAGIC.end(),
                header.begin()) ||
            header[4] != VERSION)
        {
            return false;
        }

        const std::uint32_t size =
            (static_cast<std::uint32_t>(header[6]) << 24) |
            (static_cast<std::uint32_t>(header[7]) << 16) |
            (static_cast<std::uint32_t>(header[8]) << 8) |
            static_cast<std::uint32_t>(header[9]);

        if (size > MAX_PAYLOAD)
            return false;

        frame.type =
            static_cast<Type>(header[5]);

        frame.payload.resize(size);

        return size == 0 ||
               transfer(
                   fd,
                   frame.payload.data(),
                   size,
                   false,
                   timeoutMs,
                   deadline);
    }

    bool deriveSessionKey(
        const std::array<std::uint8_t, SECRET_SIZE> &secret,
        const std::array<std::uint8_t, NONCE_SIZE> &serverNonce,
        const std::array<std::uint8_t, NONCE_SIZE> &clientNonce,
        std::array<std::uint8_t, KEY_SIZE> &key)
    {
        return hkdfExtractAndExpand(
            secret,
            serverNonce,
            clientNonce,
            SESSION_KDF_INFO,
            key);
    }

    std::string deriveMediaPassword(
        const std::array<std::uint8_t, KEY_SIZE> &sessionKey)
    {
        std::array<std::uint8_t, 32> credential{};

        if (!hmac(
                sessionKey.data(),
                sessionKey.size(),
                reinterpret_cast<const std::uint8_t *>(
                    MEDIA_CREDENTIAL_DOMAIN),
                sizeof(MEDIA_CREDENTIAL_DOMAIN) - 1,
                credential))
        {
            return {};
        }

        const std::string result =
            encodeHex(
                credential.data(),
                credential.size());

        OPENSSL_cleanse(
            credential.data(),
            credential.size());

        return result;
    }

    bool makeProof(
        const std::array<std::uint8_t, SECRET_SIZE> &secret,
        const std::string &domain,
        const std::array<std::uint8_t, NONCE_SIZE> &serverNonce,
        const std::array<std::uint8_t, NONCE_SIZE> &clientNonce,
        std::array<std::uint8_t, 32> &proof)
    {
        std::vector<std::uint8_t> input(
            domain.begin(),
            domain.end());

        input.insert(
            input.end(),
            serverNonce.begin(),
            serverNonce.end());

        input.insert(
            input.end(),
            clientNonce.begin(),
            clientNonce.end());

        const bool result =
            hmac(
                secret.data(),
                secret.size(),
                input.data(),
                input.size(),
                proof);

        OPENSSL_cleanse(
            input.data(),
            input.size());

        return result;
    }

    bool constantTimeEqual(
        const std::uint8_t *left,
        const std::uint8_t *right,
        std::size_t size)
    {
        return CRYPTO_memcmp(
                   left,
                   right,
                   size) == 0;
    }

    AuthenticatedChannel::AuthenticatedChannel(
        int fd,
        const std::array<std::uint8_t, KEY_SIZE> &sessionKey,
        bool serverSide)
        : fd(fd),
          serverSide(serverSide)
    {
        const char *sendInfo =
            serverSide
                ? SERVER_TO_CLIENT_KDF_INFO
                : CLIENT_TO_SERVER_KDF_INFO;

        const char *receiveInfo =
            serverSide
                ? CLIENT_TO_SERVER_KDF_INFO
                : SERVER_TO_CLIENT_KDF_INFO;

        if (
            !hkdfExpand(
                sessionKey,
                sendInfo,
                sendKey) ||
            !hkdfExpand(
                sessionKey,
                receiveInfo,
                receiveKey))
        {
            OPENSSL_cleanse(
                sendKey.data(),
                sendKey.size());

            OPENSSL_cleanse(
                receiveKey.data(),
                receiveKey.size());

            throw std::runtime_error(
                "Failed to derive control-channel keys");
        }
    }

    bool AuthenticatedChannel::send(
        Type type,
        const std::vector<std::uint8_t> &payload,
        int timeoutMs)
    {
        if (sendSequence == UINT64_MAX)
            return false;

        if (
            payload.size() + ENCRYPTED_OVERHEAD >
            MAX_PAYLOAD)
        {
            return false;
        }

        const auto sequence =
            sequenceBytes(sendSequence);

        const std::size_t encryptedSize =
            payload.size() + GCM_TAG_SIZE;

        const std::size_t wirePayloadSize =
            sequence.size() + encryptedSize;

        const auto header =
            makeHeader(
                type,
                wirePayloadSize);

        const auto nonce =
            makeGcmNonce(
                serverSide
                    ? SERVER_NONCE_PREFIX
                    : CLIENT_NONCE_PREFIX,
                sequence);

        std::vector<std::uint8_t> encrypted;

        if (!encryptGcm(
                sendKey,
                nonce,
                header.data(),
                header.size(),
                payload,
                encrypted))
        {
            return false;
        }

        std::vector<std::uint8_t> wire;
        wire.reserve(
            sequence.size() +
            encrypted.size());

        wire.insert(
            wire.end(),
            sequence.begin(),
            sequence.end());

        wire.insert(
            wire.end(),
            encrypted.begin(),
            encrypted.end());

        if (!sendFrame(
                fd,
                type,
                wire,
                timeoutMs))
        {
            return false;
        }

        ++sendSequence;
        return true;
    }

    bool AuthenticatedChannel::receive(
        Frame &frame,
        int timeoutMs)
    {
        if (receiveSequence == UINT64_MAX)
            return false;

        Frame wire{};

        if (
            !receiveFrame(
                fd,
                wire,
                timeoutMs) ||
            wire.payload.size() <
                ENCRYPTED_OVERHEAD)
        {
            return false;
        }

        std::array<std::uint8_t, 8> sequence{};

        std::copy_n(
            wire.payload.begin(),
            sequence.size(),
            sequence.begin());

        std::uint64_t receivedSequence = 0;

        for (const auto byte : sequence)
        {
            receivedSequence =
                (receivedSequence << 8) |
                byte;
        }

        if (
            receivedSequence !=
            receiveSequence)
        {
            return false;
        }

        const std::size_t ciphertextSize =
            wire.payload.size() -
            sequence.size() -
            GCM_TAG_SIZE;

        const auto header =
            makeHeader(
                wire.type,
                wire.payload.size());

        const auto nonce =
            makeGcmNonce(
                serverSide
                    ? CLIENT_NONCE_PREFIX
                    : SERVER_NONCE_PREFIX,
                sequence);

        const auto *ciphertext =
            wire.payload.data() +
            sequence.size();

        const auto *tag =
            ciphertext +
            ciphertextSize;

        std::vector<std::uint8_t> plaintext;

        if (!decryptGcm(
                receiveKey,
                nonce,
                header.data(),
                header.size(),
                ciphertext,
                ciphertextSize,
                tag,
                plaintext))
        {
            return false;
        }

        frame.type = wire.type;
        frame.payload = std::move(plaintext);

        ++receiveSequence;

        return true;
    }
}