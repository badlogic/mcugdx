ºcmake_minimum_required(VERSION 3.16)

if(DEFINED ESP_PLATFORM)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
endif()

project(files)

if(NOT DEFINED ESP_PLATFORM)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_subdirectory(../../ ${CMAKE_BINARY_DIR}/mcugdx)
add_executable(files "main/main.cpp")
target_link_libraries(files PUBLIC mcugdx)
mcugdx_create_rofs_partition(rofs "${CMAKE_CURRENT_SOURCE_DIR}/data/")
endif()