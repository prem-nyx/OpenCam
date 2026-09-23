#include "v4l2_device.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace
{
    constexpr const char* DEVICE_CLASS_PATH =
        "/sys/class/video4linux";

    constexpr const char* OPENCAM_DEVICE_NAME =
        "OpenCam";
}

std::string findOpenCamDevice()
{
    namespace fs = std::filesystem;

    const fs::path classPath(DEVICE_CLASS_PATH);

    if (!fs::exists(classPath))
    {
        return {};
    }

    for (const auto& entry : fs::directory_iterator(classPath))
    {
        if (!entry.is_directory() && !entry.is_symlink())
        {
            continue;
        }

        const fs::path namePath =
            entry.path() / "name";

        std::ifstream nameFile(namePath);

        if (!nameFile)
        {
            continue;
        }

        std::string deviceName;
        std::getline(nameFile, deviceName);

        if (deviceName == OPENCAM_DEVICE_NAME)
        {
            return "/dev/" + entry.path().filename().string();
        }
    }

    return {};
}

bool loadV4L2Loopback()
{
    const int result = std::system(
        "modprobe v4l2loopback "
        "devices=1 "
        "card_label=OpenCam "
        "exclusive_caps=1"
    );

    return result == 0;
}
