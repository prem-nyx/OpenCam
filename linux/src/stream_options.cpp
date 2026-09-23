#include "stream_options.hpp"

#include <cstdint>
#include <vector>

namespace
{
    void writeInt32(
        std::vector<std::uint8_t>& buffer,
        std::int32_t value)
    {
        const std::uint32_t unsignedValue =
            static_cast<std::uint32_t>(value);

        buffer.push_back(
            static_cast<std::uint8_t>((unsignedValue >> 24) & 0xFF));

        buffer.push_back(
            static_cast<std::uint8_t>((unsignedValue >> 16) & 0xFF));

        buffer.push_back(
            static_cast<std::uint8_t>((unsignedValue >> 8) & 0xFF));

        buffer.push_back(
            static_cast<std::uint8_t>(unsignedValue & 0xFF));
    }

    void writeBool(
        std::vector<std::uint8_t>& buffer,
        bool value)
    {
        buffer.push_back(value ? 1 : 0);
    }

    void writeUint16(
        std::vector<std::uint8_t>& buffer,
        std::uint16_t value)
    {
        buffer.push_back(
            static_cast<std::uint8_t>((value >> 8) & 0xFF));

        buffer.push_back(
            static_cast<std::uint8_t>(value & 0xFF));
    }
}

std::vector<std::uint8_t> buildActivationPacket(
    const StreamOptions& options)
{
    std::vector<std::uint8_t> packet;

    packet.reserve(41);

    // Packet type: ACTIVATION
    packet.push_back(0x02);

    // Stream options
    writeInt32(packet, options.fps);

    writeInt32(packet, options.width);
    writeInt32(packet, options.height);

    writeInt32(packet, options.camera);

    writeBool(packet, options.adaptiveBitrate);

    writeInt32(packet, options.bitrate);
    writeInt32(packet, options.minBitrate);
    writeInt32(packet, options.maxBitrate);

    writeBool(packet, options.stabilization);
    writeBool(packet, options.flash);
    writeBool(packet, options.h265);

    writeInt32(packet, options.focusMode);

    // No correction/effect filters for initial activation.
    writeUint16(packet, 0);

    // Empty active effect filter.
    writeUint16(packet, 0);

    return packet;
}
