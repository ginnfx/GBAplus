cmake_minimum_required(VERSION 3.20)
project(gba_emu VERSION 0.3.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The rendering pipelines have hot loops; default to an optimized build
# (Release implies -O3 on the toolchain).
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(ENABLE_TRACE_LOG "Enable TRACE_LOG output" OFF)

# SDL2 is vendored and built statically from source so the distributed app is
# self-contained (no system 'libSDL2' dependency on any platform). Pinned to
# match the version developed against.
include(FetchContent)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
set(SDL2_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
FetchContent_Declare(
    SDL2
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-2.32.10
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(SDL2)

add_library(gba_core STATIC
    src/Bus.cpp
    src/ARM7TDMI.cpp
    src/ARM7TDMI_swi.cpp
    src/PPU.cpp
    src/DMA.cpp
    src/Timers.cpp
    src/APU.cpp
    src/Emulator.cpp
    src/SaveState.cpp
)
target_include_directories(gba_core PUBLIC src)
if(ENABLE_TRACE_LOG)
    target_compile_definitions(gba_core PUBLIC ENABLE_TRACE_LOG)
endif()

# Dear ImGui (vendored, third_party/imgui) with the SDL2 + SDLRenderer2
# backends. Linked only into the GUI frontend; gba_core/tests/diag stay UI-free.
# Not under our -Wall/-Wextra below (third-party source).
add_library(imgui STATIC
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_sdl2.cpp
    third_party/imgui/backends/imgui_impl_sdlrenderer2.cpp
)
target_include_directories(imgui PUBLIC
    third_party/imgui
    third_party/imgui/backends
)
target_link_libraries(imgui PUBLIC SDL2::SDL2-static)

# tinyfiledialogs (vendored, public-domain/zlib) provides the native Open-ROM
# dialog. Plain C; kept out of our warning flags.
add_library(tinyfiledialogs STATIC
    third_party/tinyfiledialogs/tinyfiledialogs.c
)
target_include_directories(tinyfiledialogs PUBLIC third_party/tinyfiledialogs)
if(WIN32)
    target_link_libraries(tinyfiledialogs PUBLIC comdlg32 ole32 user32 shell32)
endif()

# stb_image (vendored, public domain) decodes cover-art PNG/JPEG for the game
# library grid. Header-only impl in its own TU; kept out of our warning flags.
add_library(stb_image STATIC third_party/stb/stb_image_impl.c)
target_include_directories(stb_image PUBLIC third_party/stb)

# --- GUI frontend: a real app bundle on macOS, a windowed exe on Windows. -----
set(gba_emu_platform_sources "")
if(APPLE)
    set(gba_emu_icon "${CMAKE_SOURCE_DIR}/assets/icon.icns")
    set(gba_emu_platform_sources "${gba_emu_icon}")
elseif(WIN32)
    set(gba_emu_platform_sources "${CMAKE_SOURCE_DIR}/assets/app.rc")
endif()

add_executable(gba_emu MACOSX_BUNDLE WIN32
    src/main.cpp
    src/Frontend.cpp
    src/Config.cpp
    ${gba_emu_platform_sources}
)
target_link_libraries(gba_emu PRIVATE
    gba_core
    imgui
    tinyfiledialogs
    stb_image
    SDL2::SDL2-static
    SDL2::SDL2main
)
# Single source of truth for the version the app reports (Help > About).
target_compile_definitions(gba_emu PRIVATE GBA_EMU_VERSION="${PROJECT_VERSION}")

if(APPLE)
    set_target_properties(gba_emu PROPERTIES
        OUTPUT_NAME "GBA Emu"
        MACOSX_BUNDLE_BUNDLE_NAME "GBA Emu"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.gbaplus.gba_emu"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_ICON_FILE "icon.icns"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/cmake/Info.plist.in"
    )
    set_source_files_properties("${gba_emu_icon}" PROPERTIES
        MACOSX_PACKAGE_LOCATION "Resources")
    # Ad-hoc sign so Gatekeeper allows a local launch. Distribution beyond this
    # machine needs a Developer ID + notarization (out of scope).
    add_custom_command(TARGET gba_emu POST_BUILD
        COMMAND codesign --force --deep --sign - "$<TARGET_BUNDLE_DIR:gba_emu>"
        VERBATIM)
endif()

add_executable(gba_test_harness src/test_main.cpp)
target_link_libraries(gba_test_harness PRIVATE gba_core)

add_executable(gba_diag src/diag_main.cpp)
target_link_libraries(gba_diag PRIVATE gba_core)

foreach(target gba_core gba_emu gba_test_harness)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra)
    endif()
endforeach()

# --- Install + packaging ------------------------------------------------------
if(APPLE)
    install(TARGETS gba_emu BUNDLE DESTINATION .)
elseif(WIN32)
    install(TARGETS gba_emu RUNTIME DESTINATION .)
else()
    install(TARGETS gba_emu RUNTIME DESTINATION bin)
    install(FILES assets/gba_emu.desktop DESTINATION share/applications)
    install(FILES assets/icon.png
            DESTINATION share/icons/hicolor/256x256/apps RENAME gba_emu.png)
endif()

set(CPACK_PACKAGE_NAME "gba_emu")
set(CPACK_PACKAGE_VENDOR "gbaplus")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "A Game Boy Advance emulator")
if(APPLE)
    set(CPACK_GENERATOR "DragNDrop")
elseif(WIN32)
    set(CPACK_GENERATOR "ZIP")
else()
    set(CPACK_GENERATOR "TGZ")
endif()
include(CPack)
