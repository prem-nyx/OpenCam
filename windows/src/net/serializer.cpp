#include "net/serializer.h"

#include <stdexcept>
#include <string>
#include <cstring>
#include <utility>

namespace
{
    constexpr size_t MAX_MESSAGE = 64 * 1024;
    constexpr size_t MAX_STRING = 2048;
    constexpr uint16_t MAX_LIST = 128;
    constexpr uint16_t MAX_DIMENSION = 8192;

    bool ValidUtf8(const std::string& value)
    {
        size_t i = 0;
        while (i < value.size())
        {
            const uint8_t first = static_cast<uint8_t>(value[i++]);
            if (first <= 0x7f) continue;
            size_t count = 0;
            uint32_t codepoint = 0;
            if ((first & 0xe0) == 0xc0) { count = 1; codepoint = first & 0x1f; }
            else if ((first & 0xf0) == 0xe0) { count = 2; codepoint = first & 0x0f; }
            else if ((first & 0xf8) == 0xf0) { count = 3; codepoint = first & 0x07; }
            else return false;
            if (i + count > value.size()) return false;
            for (size_t n = 0; n < count; ++n)
            {
                const uint8_t next = static_cast<uint8_t>(value[i++]);
                if ((next & 0xc0) != 0x80) return false;
                codepoint = (codepoint << 6) | (next & 0x3f);
            }
            if ((count == 1 && codepoint < 0x80) ||
                (count == 2 && codepoint < 0x800) ||
                (count == 3 && codepoint < 0x10000) ||
                codepoint > 0x10ffff || (codepoint >= 0xd800 && codepoint <= 0xdfff)) return false;
        }
        return true;
    }

    class Reader
    {
    public:
        Reader(const uint8_t* bytes, size_t size) : bytes(bytes), size(size) {}
        uint8_t u8()
        {
            require(1);
            return bytes[offset++];
        }
        uint16_t u16()
        {
            require(2);
            const uint16_t value = (static_cast<uint16_t>(bytes[offset]) << 8) |
                                   static_cast<uint16_t>(bytes[offset + 1]);
            offset += 2;
            return value;
        }
        uint32_t u32()
        {
            require(4);
            uint32_t value = 0;
            for (int i = 0; i < 4; ++i) value = (value << 8) | bytes[offset++];
            return value;
        }
        std::string string(size_t limit = MAX_STRING)
        {
            const uint16_t length = u16();
            if (length > limit) throw std::runtime_error("String exceeds protocol limit");
            require(length);
            std::string value(reinterpret_cast<const char*>(bytes + offset), length);
            offset += length;
            if (!ValidUtf8(value)) throw std::runtime_error("Invalid UTF-8");
            for (unsigned char character : value)
                if (character < 0x20 || character == 0x7f)
                    throw std::runtime_error("Control character in string");
            return value;
        }
        size_t position() const { return offset; }
        size_t remaining() const { return size - offset; }
        void require(size_t amount) const
        {
            if (amount > size - offset) throw Serializer::IncompleteMessage();
        }
        void finish() const
        {
            if (offset != size) throw std::runtime_error("Trailing protocol bytes");
        }
    private:
        const uint8_t* bytes;
        size_t size;
        size_t offset = 0;
    };

    void WriteInt16(std::vector<uint8_t>& buffer, uint16_t value)
    {
        buffer.push_back(static_cast<uint8_t>(value >> 8));
        buffer.push_back(static_cast<uint8_t>(value));
    }
    void WriteInt32(std::vector<uint8_t>& buffer, uint32_t value)
    {
        buffer.push_back(static_cast<uint8_t>(value >> 24));
        buffer.push_back(static_cast<uint8_t>(value >> 16));
        buffer.push_back(static_cast<uint8_t>(value >> 8));
        buffer.push_back(static_cast<uint8_t>(value));
    }
    void WriteBool(std::vector<uint8_t>& buffer, bool value)
    {
        buffer.push_back(value ? 1 : 0);
    }
    void WriteString(std::vector<uint8_t>& buffer, const std::string& value)
    {
        if (value.size() > MAX_STRING) throw std::length_error("String too long");
        WriteInt16(buffer, static_cast<uint16_t>(value.size()));
        buffer.insert(buffer.end(), value.begin(), value.end());
    }

    bool ValidLoopbackMediaUrl(const std::string& url)
    {
        constexpr const char* prefix = "rtsp://opencam:";
        if (url.rfind(prefix, 0) != 0) return false;
        const size_t at = url.find('@', std::strlen(prefix));
        if (at == std::string::npos || url.find('@', at + 1) != std::string::npos) return false;
        const std::string credential = url.substr(std::strlen(prefix), at - std::strlen(prefix));
        if (credential.size() != 43) return false;
        for (unsigned char c : credential)
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
        constexpr const char* endpoint = "127.0.0.1:8554/live/";
        const std::string remainder = url.substr(at + 1);
        if (remainder.rfind(endpoint, 0) != 0 || remainder.size() != std::strlen(endpoint) + 32)
            return false;
        for (size_t i = std::strlen(endpoint); i < remainder.size(); ++i)
            if (!((remainder[i] >= '0' && remainder[i] <= '9') ||
                  (remainder[i] >= 'a' && remainder[i] <= 'f'))) return false;
        return true;
    }
}

namespace Serializer
{
    DeviceDescriptor DeserializeDeviceDescriptor(const uint8_t* bytes, size_t size)
    {
        if (size > MAX_MESSAGE) throw std::runtime_error("Descriptor exceeds limit");
        Reader reader(bytes, size);
        const auto name = reader.string(256);
        const auto url = reader.string(MAX_STRING);
        if (name.empty() || !ValidLoopbackMediaUrl(url))
            throw std::runtime_error("Invalid legacy ADB descriptor");

        const uint16_t frontCount = reader.u16();
        if (frontCount > MAX_LIST) throw std::runtime_error("Too many resolutions");
        std::vector<DeviceDescriptor::Resolution> front;
        front.reserve(frontCount);
        for (uint16_t i = 0; i < frontCount; ++i)
        {
            const uint16_t width = reader.u16(), height = reader.u16();
            if (width == 0 || height == 0 || width > MAX_DIMENSION || height > MAX_DIMENSION)
                throw std::runtime_error("Invalid resolution");
            front.emplace_back(width, height);
        }

        const uint16_t backCount = reader.u16();
        if (backCount > MAX_LIST) throw std::runtime_error("Too many resolutions");
        std::vector<DeviceDescriptor::Resolution> back;
        back.reserve(backCount);
        for (uint16_t i = 0; i < backCount; ++i)
        {
            const uint16_t width = reader.u16(), height = reader.u16();
            if (width == 0 || height == 0 || width > MAX_DIMENSION || height > MAX_DIMENSION)
                throw std::runtime_error("Invalid resolution");
            back.emplace_back(width, height);
        }

        const uint16_t filterCount = reader.u16();
        if (filterCount > MAX_LIST) throw std::runtime_error("Too many filters");
        Video::Filter::Registry filters;
        for (uint16_t i = 0; i < filterCount; ++i)
        {
            const auto filterName = reader.string(128);
            const auto category = reader.u8();
            if (category > Video::Filter::ARTISTIC || filterName.empty())
                throw std::runtime_error("Invalid filter");
            filters[static_cast<Video::Filter::Category>(category)].push_back(filterName);
        }
        reader.finish();
        return DeviceDescriptor(name, url, "tcp", std::move(front), std::move(back), std::move(filters));
    }

    std::vector<uint8_t> SerializeStreamOptions(const StreamOptions& state)
    {
        if (state.fps < 1 || state.fps > 60 || state.resolution.first < 160 ||
            state.resolution.second < 160 || state.resolution.first > MAX_DIMENSION ||
            state.resolution.second > MAX_DIMENSION ||
            static_cast<int64_t>(state.resolution.first) * state.resolution.second > 16'777'216 ||
            state.focusMode < 0 || state.focusMode > 1 || state.filterSliderValues.size() > MAX_LIST ||
            state.bitrate < 64 || state.bitrate > 25000 ||
            state.minBitrate < 64 || state.minBitrate > 25000 ||
            state.maxBitrate < state.minBitrate || state.maxBitrate > 25000)
            throw std::invalid_argument("Invalid stream options");
        std::vector<uint8_t> buffer;
        buffer.reserve(256);
        buffer.push_back(0); // Replaced with ACTIVATION by the caller.
        WriteInt32(buffer, static_cast<uint32_t>(state.fps));
        WriteInt32(buffer, static_cast<uint32_t>(state.resolution.first));
        WriteInt32(buffer, static_cast<uint32_t>(state.resolution.second));
        WriteInt32(buffer, state.backCameraActive ? 1 : 0);
        WriteBool(buffer, state.adaptiveBitrate);
        // Windows settings/UI use kbit/s; Android's canonical activation
        // schema stores bit/s, matching Linux's wire values.
        WriteInt32(buffer, static_cast<uint32_t>(state.bitrate * 1024));
        WriteInt32(buffer, static_cast<uint32_t>(state.minBitrate * 1024));
        WriteInt32(buffer, static_cast<uint32_t>(state.maxBitrate * 1024));
        WriteBool(buffer, state.stabilizationEnabled);
        WriteBool(buffer, state.flashEnabled);
        WriteBool(buffer, state.h265Enabled);
        WriteInt32(buffer, static_cast<uint32_t>(state.focusMode));
        WriteInt16(buffer, static_cast<uint16_t>(state.filterSliderValues.size()));
        for (const auto& [name, value] : state.filterSliderValues)
        {
            if (value < -100 || value > 100) throw std::invalid_argument("Invalid filter value");
            WriteString(buffer, name);
            WriteInt32(buffer, static_cast<uint32_t>(value));
        }
        WriteString(buffer, state.activeEffectFilter);
        if (buffer.size() > MAX_MESSAGE) throw std::length_error("Activation exceeds limit");
        return buffer;
    }

    Connection::ErrorReport DeserializeErrorReport(const uint8_t* bytes, size_t size,
                                                   size_t& consumed)
    {
        if (size > MAX_MESSAGE) throw std::runtime_error("Error frame exceeds limit");
        Reader reader(bytes, size);
        Connection::ErrorReport report{};
        report.severity = reader.u8();
        if (report.severity > Connection::ErrorReport::SERVERITY_ERROR)
            throw std::runtime_error("Invalid error severity");
        report.error = reader.string(1024);
        report.description = reader.string(2048);
        consumed = reader.position();
        return report;
    }
}
