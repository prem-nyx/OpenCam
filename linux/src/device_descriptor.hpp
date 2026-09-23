#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct Resolution
{
    std::uint16_t width;
    std::uint16_t height;
};

struct Filter
{
    std::string name;
    std::uint8_t category;
};

struct DeviceDescriptor
{
    std::string name;
    std::string rtspUrl;

    std::vector<Resolution> frontResolutions;
    std::vector<Resolution> backResolutions;

    std::vector<Filter> filters;
};

DeviceDescriptor parseDeviceDescriptor(
    const std::vector<std::uint8_t>& data);
