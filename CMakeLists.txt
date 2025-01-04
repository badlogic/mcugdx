if(DEFINED ESP_PLATFORM)
    idf_component_register(
        SRC_DIRS "src/common" "src/esp-idf" "src/common/thirdparty/helix/src" "src/common/thirdparty/helix/src/libhelix" "src/common/thirdparty/helix/src/libhelix/real"
        INCLUDE_DIRS "src" "src/common/thirdparty/helix/src" "src/common/thirdparty/helix/src/libhelix/pub"
        REQUIRES driver esp_timer esp_partition nvs_flash esp_adc
    )
else()
cmake_minimum_required(VERSION 3.16)
project(mcugdx)

set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

include(FetchContent)
set(FETCHCONTENT_QUIET NO)
set(SDL_SHARED OFF)
set(SDL_STATIC ON)
set(SDL_TEST OFF)
set(SDL_EXAMPLES OFF)
set(SDL_INSTALL OFF)
set(SDL_TESTS OFF)
set(SDL_TESTS_BUILD OFF)
set(SDL_TESTS_BUILD_C OFF)
set(SDL_TESTS_BUILD_CPP OFF)
set(SDL_TESTS_BUILD_JOYSTICK OFF)
set(SDL_TESTS_BUILD_RENDERER OFF)
FetchContent_Declare(SDL GIT_REPOSITORY https://github.com/libsdl-org/SDL GIT_TAG release-2.30.2)
FetchContent_MakeAvailable(SDL)

if(CMAKE_BUILD_TYPE MATCHES Release)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT result)
    if(result)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
endif()

if(MSVC)
    add_compile_options(/DDESKTOP_BUILD /std:c++20 /W4 /wd4065 /wd4100 /wd4324 /wd4996 /wd4201 /wd4189 /wd4101 /wd4100)
else()
    add_compile_options(-DDESKTOP_BUILD -Wall -Wextra -Wpedantic -Werror -Wno-implicit-fallthrough -Wno-unused-but-set-variable -Wno-unused-variable -Wno-unused-parameter)
endif()

# Library
file(GLOB_RECURSE COMMON_SOURCES "src/common/*.c")
file(GLOB_RECURSE DESKTOP_SOURCES "src/desktop/*.c")
add_library(mcugdx  ${COMMON_SOURCES} ${DESKTOP_SOURCES})
target_link_libraries(mcugdx LINK_PUBLIC SDL2-static)
target_include_directories(mcugdx PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/common/thirdparty/helix/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/common/thirdparty/helix/src/libhelix/pub"
)
target_include_directories(mcugdx PUBLIC ${SDL2_SOURCE_DIR}/include)

if(APPLE)
    find_library(AUDIOTOOLBOX AudioToolbox)
    if(NOT AUDIOTOOLBOX)
        message(FATAL_ERROR "AudioToolbox framework not found")
    endif()
    target_link_libraries(mcugdx PUBLIC ${AUDIOTOOLBOX})
endif()

# Tools

# CMake extensions
include("${CMAKE_CURRENT_SOURCE_DIR}/project_include.cmake")

endif()