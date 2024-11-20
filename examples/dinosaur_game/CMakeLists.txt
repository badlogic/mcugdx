cmake_minimum_required(VERSION 3.16)

if(DEFINED ESP_PLATFORM)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
endif()

project(dinosaur_game)

if(NOT DEFINED ESP_PLATFORM)
add_subdirectory(../../ ${CMAKE_BINARY_DIR}/mcugdx)
add_executable(dinosaur_game "main/main.c")
target_link_libraries(dinosaur_game PUBLIC mcugdx)
mcugdx_create_rofs_partition(rofs "${CMAKE_CURRENT_SOURCE_DIR}/data/")
endif()