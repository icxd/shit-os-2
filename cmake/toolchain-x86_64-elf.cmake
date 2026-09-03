# SPDX-License-Identifier: GPL-3.0-or-later
#
# shit os 2 -- cross toolchain.
#
# There is deliberately no toolchain to bootstrap here. Clang is a cross
# compiler by construction, so pointing it at x86_64-elf and linking with
# ld.lld is the whole story: install clang and lld and you can build the OS.

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(SHITOS_TARGET_TRIPLE "x86_64-elf" CACHE STRING "Target triple for the cross build")

find_program(SHITOS_CLANG NAMES clang clang-18 clang-17 REQUIRED)
find_program(SHITOS_CLANGXX NAMES clang++ clang++-18 clang++-17 REQUIRED)
find_program(SHITOS_LLD NAMES ld.lld ld.lld-18 ld.lld-17 REQUIRED)
find_program(SHITOS_AR NAMES llvm-ar llvm-ar-18 llvm-ar-17 REQUIRED)
find_program(SHITOS_RANLIB NAMES llvm-ranlib llvm-ranlib-18 llvm-ranlib-17 REQUIRED)
find_program(SHITOS_OBJCOPY NAMES llvm-objcopy llvm-objcopy-18 llvm-objcopy-17 REQUIRED)

set(CMAKE_C_COMPILER "${SHITOS_CLANG}")
set(CMAKE_CXX_COMPILER "${SHITOS_CLANGXX}")
set(CMAKE_ASM_COMPILER "${SHITOS_CLANG}")
set(CMAKE_AR "${SHITOS_AR}")
set(CMAKE_RANLIB "${SHITOS_RANLIB}")
set(CMAKE_OBJCOPY "${SHITOS_OBJCOPY}")

set(CMAKE_C_COMPILER_TARGET "${SHITOS_TARGET_TRIPLE}")
set(CMAKE_CXX_COMPILER_TARGET "${SHITOS_TARGET_TRIPLE}")
set(CMAKE_ASM_COMPILER_TARGET "${SHITOS_TARGET_TRIPLE}")

# There is no libc to link against, so CMake's compiler probe must not try to
# produce an executable.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(CMAKE_C_FLAGS_INIT "-ffreestanding -nostdlib")
set(CMAKE_CXX_FLAGS_INIT "-ffreestanding -nostdlib")
set(CMAKE_ASM_FLAGS_INIT "-ffreestanding")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld -nostdlib -static")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
