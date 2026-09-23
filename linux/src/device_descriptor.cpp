#include "device_descriptor.hpp"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
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

        if (offset + length > data.size())
        {
            throw std::runtime_error(
                "Unexpected end of data while reading string");
        }

        std::string value(
            reinterpret_cast<const char*>(data.data() + offset),
            length);

        offset += length;

        return value;
    }

    std::vector<Resolution> readResolutions(
        const std::vector<std::uint8_t>& data,
        std::size_t& offset)
    {
        const std::uint16_t count =
            readUint16(data, offset);

        std::vector<Resolution> resolutions;
        resolutions.reserve(count);

        for (std::uint16_t i = 0; i < count; ++i)
        {
            Resolution resolution{
                readUint16(data, offset),
                readUint16(data, offset)
            };

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
    std::size_t offset = 0;

    DeviceDescriptor descriptor;

    descriptor.name =
        readString(data, offset);

    descriptor.rtspUrl =
        readString(data, offset);

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
