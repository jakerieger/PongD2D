cmake_minimum_required(VERSION 3.14)
project(PongD2D)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(PongD2D WIN32 res/resource.h res/app.rc main.cpp)

if (MSVC)
    # Disable warning C4996
    target_compile_options(PongD2D PRIVATE /wd4996)
endif ()

add_custom_command(TARGET PongD2D PRE_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/assets ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/assets)