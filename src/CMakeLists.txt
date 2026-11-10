# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) the developer
cmake_minimum_required(VERSION 3.22)
project(aaceld CXX C ASM)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

set(THIRD_PARTY ${CMAKE_CURRENT_SOURCE_DIR}/../third_party)
set(SH ${THIRD_PARTY}/shadowhook)

# The hook engine is linked in statically on purpose. A Zygisk module library
# is loaded from /data/adb, where the dynamic linker in the target process
# cannot resolve DT_NEEDED entries, so it must not depend on any shipped .so.
if(${ANDROID_ABI} STREQUAL "arm64-v8a")
    set(SH_ARCH "arm64")
elseif(${ANDROID_ABI} STREQUAL "armeabi-v7a")
    set(SH_ARCH "arm")
endif()

file(GLOB SH_SRC
    ${SH}/*.c
    ${SH}/arch/${SH_ARCH}/*.c
    ${SH}/arch/${SH_ARCH}/*.S
    ${SH}/common/*.c
    ${SH}/third_party/*/*.c
)
add_library(shadowhook_static STATIC ${SH_SRC})
target_include_directories(shadowhook_static PUBLIC
    ${SH} ${SH}/include ${SH}/arch/${SH_ARCH} ${SH}/common
    ${SH}/third_party/xdl ${SH}/third_party/bsd ${SH}/third_party/lss
)
# Upstream builds with -Weverything -Werror; this is vendored code, so it is
# compiled as-is rather than being held to this project's warning settings.
target_compile_options(shadowhook_static PRIVATE -Os -ffunction-sections -fdata-sections -w)
target_link_libraries(shadowhook_static PUBLIC log)

add_library(aaceld SHARED
    main.cpp
    target.cpp
    hooks.cpp
    eld_encoder.cpp
)

target_include_directories(aaceld PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${THIRD_PARTY}/zygisk
)

target_link_libraries(aaceld PRIVATE shadowhook_static log)

target_compile_options(aaceld PRIVATE
    -Wall -Wextra -Werror
    -fvisibility=hidden
    -ffunction-sections -fdata-sections
    -fno-exceptions -fno-rtti
)
target_link_options(aaceld PRIVATE
    -Wl,--gc-sections
    -Wl,-z,max-page-size=16384
)

# Not part of the module: runs its start-up against a copy of a stack library
# in a scratch process. See tools/harness/ and docs/PORTING.md.
add_executable(aaceld_harness
    ../tools/harness/harness.cpp
    target.cpp
    hooks.cpp
    eld_encoder.cpp
)
target_include_directories(aaceld_harness PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(aaceld_harness PRIVATE shadowhook_static log dl)
target_compile_options(aaceld_harness PRIVATE -Wall -Wextra -fno-exceptions -fno-rtti)
target_link_options(aaceld_harness PRIVATE -Wl,-z,max-page-size=16384)
