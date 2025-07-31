# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.14)

# Add WAMR as a component
set(COMPONENTS main freertos esptool_py wamr esp_psram)

# Set WAMR path - you may need to adjust this path
if(DEFINED ENV{WAMR_PATH})
    set(WAMR_ROOT $ENV{WAMR_PATH})
else()
    set(WAMR_ROOT "/home/jechternach/src/esp32/wasm-micro-runtime")
endif()

# Add WAMR component directory
list(APPEND EXTRA_COMPONENT_DIRS "${WAMR_ROOT}/build-scripts/esp-idf")

# Debug output
message(STATUS "WAMR_ROOT: ${WAMR_ROOT}")
message(STATUS "WAMR component path: ${WAMR_ROOT}/build-scripts/esp-idf")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(led-test-wamr)