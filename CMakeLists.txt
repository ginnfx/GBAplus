cmake_minimum_required(VERSION 3.20)
project(gba_emu CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The rendering pipelines are hot loops; default to an optimized build
# (Release implies -O3 with this toolchain).
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(ENABLE_TRACE_LOG "Enable TRACE_LOG output" OFF)

find_package(SDL2 REQUIRED)

add_library(gba_core STATIC
    src/Bus.cpp
    src/ARM7TDMI.cpp
    src/PPU.cpp
    src/DMA.cpp
)
target_include_directories(gba_core PUBLIC src)
if(ENABLE_TRACE_LOG)
    target_compile_definitions(gba_core PUBLIC ENABLE_TRACE_LOG)
endif()

add_executable(gba_emu src/main.cpp)
target_link_libraries(gba_emu PRIVATE gba_core SDL2::SDL2)

add_executable(gba_test_harness src/test_main.cpp)
target_link_libraries(gba_test_harness PRIVATE gba_core)

foreach(target gba_core gba_emu gba_test_harness)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra)
    endif()
endforeach()
