cmake_minimum_required(VERSION 3.10)
project(mcugdx-tools C)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# rofs
add_executable(rofs rofs.c)

if(WIN32)
    set_target_properties(rofs PROPERTIES OUTPUT_NAME "rofs-win" SUFFIX ".exe")
    target_compile_definitions(rofs PRIVATE _CRT_SECURE_NO_WARNINGS)
elseif(APPLE)
    set_target_properties(rofs PROPERTIES OUTPUT_NAME "rofs-mac")
else()
    set_target_properties(rofs PROPERTIES OUTPUT_NAME "rofs-lin")
endif()

add_custom_command(TARGET rofs POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_SOURCE_DIR}/bin/
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:rofs> ${CMAKE_SOURCE_DIR}/bin/
)