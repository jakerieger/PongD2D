cmake_minimum_required(VERSION 3.29)
project(PongD2D)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(PongD2D WIN32 res/resource.h res/app.rc main.cpp)

if (MSVC)
    # Disable warning C4996
    target_compile_options(PongD2D PRIVATE /wd4996)
endif ()