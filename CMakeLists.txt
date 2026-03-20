cmake_minimum_required(VERSION 3.16)
project(sdl3_test_chess_app C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(SDL3 REQUIRED CONFIG)

add_executable(chess_app
    src/main.c
)

target_link_libraries(chess_app PRIVATE SDL3::SDL3)
