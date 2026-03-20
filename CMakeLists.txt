cmake_minimum_required(VERSION 3.16)
project(sdl3_test_chess_app C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)
find_package(PkgConfig)

find_package(SDL3 REQUIRED CONFIG)

if(PkgConfig_FOUND)
    pkg_check_modules(AVAHI_CLIENT QUIET avahi-client)
endif()

add_executable(chess_app
    src/main.c
    src/app.c
    src/render/board.c
    src/network/peer.c
    src/network/session.c
    src/network/discovery.c
    src/network/tcp.c
)

target_include_directories(chess_app PRIVATE include)

if(AVAHI_CLIENT_FOUND)
    target_compile_definitions(chess_app PRIVATE CHESS_APP_HAVE_AVAHI=1)
    target_include_directories(chess_app PRIVATE ${AVAHI_CLIENT_INCLUDE_DIRS})
    target_link_libraries(chess_app PRIVATE ${AVAHI_CLIENT_LIBRARIES})
endif()

# DNS-SD (Bonjour) — available on macOS as part of the system SDK, no extra link needed
if(APPLE AND NOT AVAHI_CLIENT_FOUND)
    include(CheckIncludeFile)
    check_include_file("dns_sd.h" HAVE_DNS_SD_H)
    if(HAVE_DNS_SD_H)
        target_compile_definitions(chess_app PRIVATE CHESS_APP_HAVE_DNSSD=1)
    endif()
endif()

target_link_libraries(chess_app PRIVATE SDL3::SDL3 Threads::Threads)
