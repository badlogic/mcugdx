cmake_minimum_required(VERSION 3.10)
project(mcugdx-tools C)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# rofs
add_executable(rofs rofs.c)
if(WIN32)
    set_target_properties(rofs PROPERTIES OUTPUT_NAME "rofs" SUFFIX ".exe")
    target_compile_definitions(rofs PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()