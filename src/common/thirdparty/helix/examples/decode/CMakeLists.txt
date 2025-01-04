cmake_minimum_required(VERSION 3.25)

project(example_decode)

add_executable(example_decode example_decode.c)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_C_FLAGS "-Wall -Wextra -Werror")
set(CMAKE_C_FLAGS_RELEASE "-O3")

target_link_libraries(example_decode
    PRIVATE
        helix_mp3
)
