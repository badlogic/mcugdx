cmake_minimum_required(VERSION 3.16)

if(DEFINED ESP_PLATFORM)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
endif()

project(display)

if(NOT DEFINED ESP_PLATFORM)
add_subdirectory(../../ ${CMAKE_BINARY_DIR}/mcugdx)
add_executable(display "main/main.cpp")
target_link_libraries(display PUBLIC mcugdx)
mcugdx_create_rofs_partition(rofs "${CMAKE_CURRENT_SOURCE_DIR}/data/")
endif()