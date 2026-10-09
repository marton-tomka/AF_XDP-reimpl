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

add_custom_target(afxdp_build_info
    COMMAND ${CMAKE_COMMAND}
        "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}"
        "-DCOMPILER=${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}"
        "-DCONFIGURATION=$<CONFIG>"
        "-DLIBBPF_VERSION=${LIBBPF_VERSION}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/WriteBuildInfo.cmake"
    BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/generated/build_info.hpp"
    VERBATIM
)
add_dependencies(afxdp_receiver afxdp_build_info)
target_include_directories(afxdp_receiver PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")

include(CTest)
if(BUILD_TESTING)
    add_executable(afxdp_reflector_tests tests/reflector.cpp)
    target_link_libraries(afxdp_reflector_tests PRIVATE afxdp project_warnings project_sanitizers)
    add_test(NAME afxdp_reflector COMMAND afxdp_reflector_tests)
    set_tests_properties(afxdp_reflector PROPERTIES TIMEOUT 10)

    add_executable(afxdp_io_progress_tests tests/io_progress.cpp)
    target_link_libraries(afxdp_io_progress_tests PRIVATE
        afxdp project_warnings project_sanitizers
    )
    target_link_options(afxdp_io_progress_tests PRIVATE
        -Wl,--wrap=mmap,--wrap=socket,--wrap=close,--wrap=setrlimit,--wrap=mlock
        -Wl,--wrap=if_nametoindex,--wrap=bind,--wrap=setsockopt,--wrap=getsockopt
        -Wl,--wrap=sendto,--wrap=recvfrom
    )
    add_test(NAME afxdp_io_progress COMMAND afxdp_io_progress_tests)
    set_tests_properties(afxdp_io_progress PROPERTIES TIMEOUT 10)

    add_executable(afxdp_measurement_tests tests/measurement.cpp)
    target_link_libraries(afxdp_measurement_tests PRIVATE afxdp project_warnings project_sanitizers)
    target_link_options(afxdp_measurement_tests PRIVATE -Wl,--wrap=clock_gettime,--wrap=nanosleep)
    add_test(NAME afxdp_measurement COMMAND afxdp_measurement_tests)
    set_tests_properties(afxdp_measurement PROPERTIES TIMEOUT 10)

    add_executable(afxdp_lifecycle_tests tests/lifecycle.cpp)
    add_dependencies(afxdp_lifecycle_tests afxdp_build_info)
    target_include_directories(afxdp_lifecycle_tests PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
    target_link_libraries(afxdp_lifecycle_tests PRIVATE afxdp project_warnings project_sanitizers)
    target_link_options(afxdp_lifecycle_tests PRIVATE
        -Wl,--wrap=mmap,--wrap=munmap,--wrap=socket,--wrap=close,--wrap=setrlimit,--wrap=mlock
        -Wl,--wrap=if_nametoindex,--wrap=bind,--wrap=setsockopt,--wrap=getsockopt
        -Wl,--wrap=sendto,--wrap=recvfrom,--wrap=sigaction,--wrap=pthread_setaffinity_np,--wrap=sched_setscheduler
        -Wl,--wrap=clock_gettime,--wrap=nanosleep,--wrap=fopen
        -Wl,--wrap=_ZNSt6chrono3_V212steady_clock3nowEv
        -Wl,--wrap=bpf_object__open,--wrap=bpf_object__load,--wrap=bpf_object__close
        -Wl,--wrap=bpf_object__find_map_by_name,--wrap=bpf_map__fd
        -Wl,--wrap=bpf_map_update_elem,--wrap=bpf_map_delete_elem
        -Wl,--wrap=bpf_object__find_program_by_name,--wrap=bpf_program__fd
        -Wl,--wrap=bpf_xdp_attach,--wrap=bpf_xdp_detach
    )
    add_test(NAME afxdp_lifecycle COMMAND afxdp_lifecycle_tests)
    set_tests_properties(afxdp_lifecycle PROPERTIES TIMEOUT 20)
endif()
