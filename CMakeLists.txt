# -*- mode: CMake; tab-width: 2; indent-tabs-mode: nil; -*-
#---------------------------------------------------------------------------------------------------
# Copyright (C) 2022 Marcus Geelnard
#
# Redistribution and use in source and binary forms, with or without modification, are permitted
# provided that the following conditions are met:
#
#   1. Redistributions of source code must retain the above copyright notice, this list of
#      conditions and the following disclaimer.
#
#   2. Redistributions in binary form must reproduce the above copyright notice, this list of
#      conditions and the following disclaimer in the documentation and/or other materials provided
#      with the distribution.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR
# IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
# FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
# CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
# DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
# WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY
# WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#---------------------------------------------------------------------------------------------------

cmake_minimum_required(VERSION 3.10)
project(mfat LANGUAGES C)

# Enable clang tooling etc.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# We build everything against the C99 standard.
set(CMAKE_C_STANDARD 99)
set(C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Configuration options.
set(MFAT_ENABLE_DEBUG      OFF CACHE BOOL   "Enable debug printing")
set(MFAT_ENABLE_WRITE      ON  CACHE BOOL   "Enable write suport")
set(MFAT_ENABLE_MBR        ON  CACHE BOOL   "Enable MBR suport")
set(MFAT_ENABLE_GPT        ON  CACHE BOOL   "Enable GPT suport")
set(MFAT_NUM_CACHED_BLOCKS "2" CACHE STRING "Number of blocks to cache")
set(MFAT_NUM_FDS           "4" CACHE STRING "Maximum number of file descriptors")
set(MFAT_NUM_PARTITIONS    "4" CACHE STRING "Maximum number of partitions")

list(APPEND defines "MFAT_ENABLE_DEBUG=$<BOOL:${MFAT_ENABLE_DEBUG}>")
list(APPEND defines "MFAT_ENABLE_WRITE=$<BOOL:${MFAT_ENABLE_WRITE}>")
list(APPEND defines "MFAT_ENABLE_MBR=$<BOOL:${MFAT_ENABLE_MBR}>")
list(APPEND defines "MFAT_ENABLE_GPT=$<BOOL:${MFAT_ENABLE_GPT}>")
list(APPEND defines "MFAT_NUM_CACHED_BLOCKS=${MFAT_NUM_CACHED_BLOCKS}")
list(APPEND defines "MFAT_NUM_FDS=${MFAT_NUM_FDS}")
list(APPEND defines "MFAT_NUM_PARTITIONS=${MFAT_NUM_PARTITIONS}")

# Define compiler warnings.
if((CMAKE_C_COMPILER_ID STREQUAL "GNU") OR (CMAKE_C_COMPILER_ID MATCHES "Clang"))
  list(APPEND options -Wall -Wextra -Wpedantic -Werror)
elseif(MSVC)
  list(APPEND options /W4 /WX)
endif()

# The MFAT library.
add_library(mfat mfat.c mfat.h)
target_compile_definitions(mfat PRIVATE ${defines})
target_compile_options(mfat PRIVATE ${options})
target_include_directories(mfat PUBLIC .)

# Example programs.
add_subdirectory(examples)
