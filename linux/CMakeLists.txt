cmake_minimum_required(VERSION 3.16)

project(opencam
    VERSION 0.1.0
    LANGUAGES CXX
)

find_package(OpenSSL REQUIRED)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(opencam
    src/control_protocol.cpp
    src/main.cpp
    src/device_descriptor.cpp
    src/stream_options.cpp
    src/security_validation.cpp
    src/ffmpeg_runner.cpp
    src/rtsp_probe.cpp
    src/v4l2_device.cpp
    src/network.cpp
    src/qr.cpp
)

target_link_libraries(opencam PRIVATE OpenSSL::Crypto)
target_compile_options(opencam PRIVATE -Wall -Wextra -Wpedantic)

include(CTest)
if(BUILD_TESTING)
    add_executable(opencam_security_tests
        tests/security_tests.cpp
        src/control_protocol.cpp
        src/device_descriptor.cpp
        src/security_validation.cpp
    )
    target_include_directories(opencam_security_tests PRIVATE src)
    target_link_libraries(opencam_security_tests PRIVATE OpenSSL::Crypto)
    add_test(NAME opencam_security_tests COMMAND opencam_security_tests)

    add_executable(opencam_shell_injection_regression
        tests/shell_injection_regression.cpp
        src/rtsp_probe.cpp
    )
    target_include_directories(opencam_shell_injection_regression PRIVATE src)
    add_test(NAME opencam_shell_injection_regression
             COMMAND opencam_shell_injection_regression)
    set_tests_properties(opencam_shell_injection_regression PROPERTIES TIMEOUT 60)
endif()
