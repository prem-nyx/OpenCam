#include "device_descriptor.hpp"

#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
    constexpr std::size_t MAX_DESCRIPTOR_SIZE = 64 * 1024;
    constexpr std::size_t MAX_STRING_SIZE = 2048;
    constexpr std::uint16_t MAX_RESOLUTIONS = 128;
    constexpr std::uint16_t MAX_FILTERS = 128;
    constexpr std::uint16_t MAX_DIMENSION = 8192;

    bool isValidUtf8(const std::string& value)
    {
        const auto* bytes = reinterpret_cast<const unsigned char*>(value.data());
        std::size_t i = 0;
        while (i < value.size())
        {
            const unsigned char first = bytes[i++];
            if (first <= 0x7f) continue;
            std::size_t continuation = 0;
            std::uint32_t codepoint = 0;
            if ((first & 0xe0) == 0xc0) { continuation = 1; codepoint = first & 0x1f; }
            else if ((first & 0xf0) == 0xe0) { continuation = 2; codepoint = first & 0x0f; }
            else if ((first & 0xf8) == 0xf0) { continuation = 3; codepoint = first & 0x07; }
            else return false;
            if (i + continuation > value.size()) return false;
            for (std::size_t j = 0; j < continuation; ++j)
            {
                const unsigned char next = bytes[i++];
                if ((next & 0xc0) != 0x80) return false;
                codepoint = (codepoint << 6) | (next & 0x3f);
            }
            if ((continuation == 1 && codepoint < 0x80) ||
                (continuation == 2 && codepoint < 0x800) ||
                (continuation == 3 && codepoint < 0x10000) ||
                codepoint > 0x10ffff ||
                (codepoint >= 0xd800 && codepoint <= 0xdfff)) return false;
        }
        return true;
    }

    std::uint16_t readUint16(
        const std::vector<std::uint8_t>& data,
        std::size_t& offset)
    {
        if (offset + 2 > data.size())
        {
            throw std::runtime_error(
                "Unexpected end of data while reading uint16");
        }

        const std::uint16_t value =
            (static_cast<std::uint16_t>(data[offset]) << 8) |
            static_cast<std::uint16_t>(data[offset + 1]);

        offset += 2;

        return value;
    }

    std::string readString(
        const std::vector<std::uint8_t>& data,
        std::size_t& offset)
    {
        const std::uint16_t length =
            readUint16(data, offset);

        if (length > MAX_STRING_SIZE)
            throw std::runtime_error("String exceeds protocol limit");

        if (offset + length > data.size())
        {
            throw std::runtime_error(
                "Unexpected end of data while reading string");
        }

        std::string value(
            reinterpret_cast<const char*>(data.data() + offset),
            length);

        offset += length;

        if (!isValidUtf8(value))
            throw std::runtime_error("Invalid UTF-8 string");
        for (unsigned char character : value)
            if (character < 0x20 || character == 0x7f)
                throw std::runtime_error("Control character in descriptor string");

        return value;
    }

    std::vector<Resolution> readResolutions(
        const std::vector<std::uint8_t>& data,
        std::size_t& offset)
    {
        const std::uint16_t count =
            readUint16(data, offset);

        if (count > MAX_RESOLUTIONS)
            throw std::runtime_error("Too many resolutions");

        std::vector<Resolution> resolutions;
        resolutions.reserve(count);

        for (std::uint16_t i = 0; i < count; ++i)
        {
            Resolution resolution{
                readUint16(data, offset),
                readUint16(data, offset)
            };

            if (resolution.width == 0 || resolution.height == 0 ||
                resolution.width > MAX_DIMENSION ||
                resolution.height > MAX_DIMENSION)
                throw std::runtime_error("Resolution outside allowed range");

            resolutions.push_back(resolution);
        }

        return resolutions;
    }

    std::vector<Filter> readFilters(
        const std::vector<std::uint8_t>& data,
        std::size_t& offset)
    {
        const std::uint16_t count =
            readUint16(data, offset);

        if (count > MAX_FILTERS)
            throw std::runtime_error("Too many filters");

        std::vector<Filter> filters;
        filters.reserve(count);

        for (std::uint16_t i = 0; i < count; ++i)
        {
            std::string name =
                readString(data, offset);

            if (offset >= data.size())
            {
                throw std::runtime_error(
                    "Unexpected end of data while reading filter category");
            }

            const std::uint8_t category =
                data[offset];

            if (category > 4)
                throw std::runtime_error("Unknown filter category");

            ++offset;

            filters.push_back({
                name,
                category
            });
        }

        return filters;
    }
}

DeviceDescriptor parseDeviceDescriptor(
    const std::vector<std::uint8_t>& data)
{
    if (data.size() > MAX_DESCRIPTOR_SIZE)
        throw std::runtime_error("Descriptor exceeds protocol limit");

    std::size_t offset = 0;

    DeviceDescriptor descriptor;

    descriptor.name =
        readString(data, offset);

    descriptor.rtspUrl =
        readString(data, offset);

    if (descriptor.name.empty() || descriptor.rtspUrl.empty())
        throw std::runtime_error("Required descriptor field is empty");

    descriptor.frontResolutions =
        readResolutions(data, offset);

    descriptor.backResolutions =
        readResolutions(data, offset);

    descriptor.filters =
        readFilters(data, offset);

    if (offset != data.size())
    {
        throw std::runtime_error(
            "Unexpected trailing data in DeviceDescriptor");
    }

    return descriptor;
}
