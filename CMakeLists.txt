# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Marton Tomka


#cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
#cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug
#cmake -S . -B build/asan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DAFXDP_SANITIZER=address
#cmake -S . -B build/tsan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DAFXDP_SANITIZER=thread


cmake_minimum_required(VERSION 3.20)
project(afxdp_receiver LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "AF_XDP is Linux-only")
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(AFXDP_SANITIZER "none" CACHE STRING "Sanitizer: none, address, or thread")
set_property(CACHE AFXDP_SANITIZER PROPERTY STRINGS none address thread)

if(NOT AFXDP_SANITIZER MATCHES "^(none|address|thread)$")
    message(FATAL_ERROR
        "Invalid AFXDP_SANITIZER='${AFXDP_SANITIZER}'; expected none, address, or thread"
    )
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 14.0)
    message(FATAL_ERROR "GCC >= 14 is required (std::print / std::expected). Found ${CMAKE_CXX_COMPILER_VERSION}.")
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBBPF REQUIRED IMPORTED_TARGET libbpf)
find_package(Threads REQUIRED)

add_library(afxdp INTERFACE)
target_compile_features(afxdp INTERFACE cxx_std_23)
target_include_directories(afxdp INTERFACE "${PROJECT_SOURCE_DIR}/include/afxdp")
target_link_libraries(afxdp INTERFACE PkgConfig::LIBBPF Threads::Threads)

add_library(project_warnings INTERFACE)
target_compile_options(project_warnings INTERFACE
    "$<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>"
)

add_library(project_sanitizers INTERFACE)

if(AFXDP_SANITIZER STREQUAL "address")
    set(sanitizer_flags -fsanitize=address,undefined)
elseif(AFXDP_SANITIZER STREQUAL "thread")
    set(sanitizer_flags -fsanitize=thread,undefined)
endif()

if(sanitizer_flags)
    target_compile_options(project_sanitizers INTERFACE
        ${sanitizer_flags}
        -fno-omit-frame-pointer
        -g3
    )
    target_link_options(project_sanitizers INTERFACE ${sanitizer_flags})
endif()

add_executable(afxdp_receiver main.cpp)
target_link_libraries(afxdp_receiver PRIVATE
    afxdp
    project_warnings
    project_sanitizers
)

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
