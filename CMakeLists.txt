# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.24)

# Select the cross toolchain before project() unless the caller already picked
# one, so that a bare `cmake -B build -G Ninja` does the right thing.
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_SOURCE_DIR}/cmake/toolchain-x86_64-elf.cmake"
        CACHE FILEPATH "Cross toolchain")
endif()

project(shit-os-2 VERSION 2.0.0 LANGUAGES C CXX ASM)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

set(SHITOS_ARCH "x86_64" CACHE STRING "Target architecture")
set(SHITOS_ABI_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/include")

# Flags shared by absolutely everything we compile: kernel, modules, libc and
# userland all run without a hosted runtime underneath them.
add_library(shitos_freestanding INTERFACE)
target_compile_options(shitos_freestanding INTERFACE
    -ffreestanding
    -fno-stack-protector
    -fno-strict-aliasing
    -fno-common
    -Wall -Wextra
    -Wno-unused-parameter
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-threadsafe-statics>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-use-cxa-atexit>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-unwind-tables>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-asynchronous-unwind-tables>
    $<$<CONFIG:Debug>:-g>
    $<$<CONFIG:Debug>:-O1>
    $<$<CONFIG:Release>:-O2>
)
target_include_directories(shitos_freestanding INTERFACE "${SHITOS_ABI_INCLUDE}")

# Additional flags for anything that runs in ring 0. The kernel has no FPU
# state to save on entry, and the red zone is not safe when interrupts can fire
# on the current stack at any instruction.
add_library(shitos_kernel_flags INTERFACE)
target_link_libraries(shitos_kernel_flags INTERFACE shitos_freestanding)
target_compile_options(shitos_kernel_flags INTERFACE
    -mno-red-zone
    -mno-mmx
    -mno-sse
    -mno-sse2
    -mno-80387
    -fno-pic
    -fno-pie
)
target_compile_definitions(shitos_kernel_flags INTERFACE SHITOS_KERNEL=1)

add_subdirectory(kernel)
add_subdirectory(modules)
add_subdirectory(user)
add_subdirectory(ports)

include(cmake/image.cmake)
