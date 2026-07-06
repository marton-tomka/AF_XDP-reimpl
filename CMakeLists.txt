# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Marton Tomka

cmake_minimum_required(VERSION 3.20)
project(afxdp_receiver LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "AF_XDP is Linux-only")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 14.0)
    message(FATAL_ERROR "GCC >= 14 is required (std::print / std::expected). Found ${CMAKE_CXX_COMPILER_VERSION}.")
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBBPF REQUIRED IMPORTED_TARGET libbpf)
find_package(Threads REQUIRED)

add_executable(afxdp_receiver main.cpp)
target_include_directories(afxdp_receiver PRIVATE include/afxdp)
target_compile_options(afxdp_receiver PRIVATE -Wall -Wextra)
target_link_libraries(afxdp_receiver PRIVATE PkgConfig::LIBBPF Threads::Threads)

find_program(CLANG_EXECUTABLE clang REQUIRED)

set(BPF_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/bpf/xdp_prog_bpf.c)
set(BPF_OBJECT ${CMAKE_CURRENT_BINARY_DIR}/xdp_prog.bpf.o)

add_custom_command(
    OUTPUT ${BPF_OBJECT}
    COMMAND ${CLANG_EXECUTABLE} -O2 -g -target bpf -D__TARGET_ARCH_x86
            -I/usr/include/x86_64-linux-gnu
            -c ${BPF_SOURCE} -o ${BPF_OBJECT}
    DEPENDS ${BPF_SOURCE}
    COMMENT "Building xdp_prog.bpf.o (BPF target)"
    VERBATIM
)

add_custom_target(xdp_prog ALL DEPENDS ${BPF_OBJECT})
add_dependencies(afxdp_receiver xdp_prog)
