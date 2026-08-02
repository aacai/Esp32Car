cmake_minimum_required(VERSION 3.16)

# 引入 ESP-IDF 的 CMake 构建系统
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# 工程名（生成的固件为 build/esp32c3_iot.bin / .elf）
project(esp32c3_iot)
