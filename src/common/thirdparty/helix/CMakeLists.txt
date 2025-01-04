cmake_minimum_required(VERSION 3.25)

project(helix_mp3)

file(GLOB_RECURSE HELIX_MP3_SRCS CONFIGURE_DEPENDS 
    "${PROJECT_SOURCE_DIR}/src/*.c"
)

add_library(helix_mp3 STATIC)

option(BUILD_EXAMPLES "Build examples" OFF)

target_sources(helix_mp3
    PRIVATE
        ${HELIX_MP3_SRCS}
)

target_include_directories(helix_mp3
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/libhelix/pub
)

# Prevent compiler from raising errors from unused variables inside libhelix code
target_compile_options(helix_mp3
    PRIVATE
        -Wno-unused-but-set-variable
)

if (BUILD_EXAMPLES)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/examples)
endif ()
