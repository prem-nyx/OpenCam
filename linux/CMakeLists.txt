cmake_minimum_required(VERSION 3.16)

project(opencam
    VERSION 0.1.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(opencam
    src/main.cpp
    src/device_descriptor.cpp
    src/stream_options.cpp
    src/ffmpeg_runner.cpp
    src/rtsp_probe.cpp
    src/v4l2_device.cpp
    src/network.cpp
    src/qr.cpp
)
