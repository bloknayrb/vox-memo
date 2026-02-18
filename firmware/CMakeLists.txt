# Vox Memo — ESP32-C6 firmware
# Requires ESP-IDF v5.3+ and Waveshare BSP components

cmake_minimum_required(VERSION 3.16)

# Waveshare BSP is used for GPIO pin reference only, not as build components.
# LVGL and esp_littlefs are pulled via idf_component.yml (IDF Component Manager).

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(vox_memo)
