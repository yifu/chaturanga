cmake_minimum_required(VERSION 3.16)
project(sdl3_test_chess_app C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)

find_package(SDL3 REQUIRED CONFIG)

add_executable(chess_app
    src/main.c
    src/app.c
    src/render/board.c
    src/network/peer.c
    src/network/session.c
)

target_include_directories(chess_app PRIVATE include)

target_link_libraries(chess_app PRIVATE SDL3::SDL3 Threads::Threads)
