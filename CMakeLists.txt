# Wolfenstein 3-D (v1.4, the Steam/GOG release), statically recompiled.
#
# wolf3d_wl6 is built only if its lifted C exists -- run `py tools/lift.py wl6`
# first (Setup.cmd does). The lifted C is generated from your own copy of the
# game and is never committed; see README.
#
#   cmake -B build -G "Visual Studio 17 2022" -A x64
#   cmake --build build --config Release
cmake_minimum_required(VERSION 3.20)
project(wolf3d C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
# Static CRT: the exe runs on a machine with no VC++ redistributable (netlab's
# test VMs, a fresh install) instead of failing to start (repo rules, section 10).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
option(WOLF_TRACE "record lifted calls and enable WOLF_WATCH (slow)" OFF)

# pcrecomp supplies the CPU model (runtime/recomp16/cpu.h) the lifted C uses.
set(PCRECOMP_HOME "$ENV{PCRECOMP_HOME}" CACHE PATH "pcrecomp checkout")
if(NOT PCRECOMP_HOME)
    foreach(cand ../pcrecomp ../pcrecomp-main)
        if(EXISTS "${CMAKE_SOURCE_DIR}/${cand}/runtime/recomp16/cpu.h")
            get_filename_component(PCRECOMP_HOME "${CMAKE_SOURCE_DIR}/${cand}" ABSOLUTE)
            break()
        endif()
    endforeach()
endif()
if(NOT EXISTS "${PCRECOMP_HOME}/runtime/recomp16/cpu.h")
    message(FATAL_ERROR "pcrecomp not found: set PCRECOMP_HOME to a checkout of https://github.com/sp00nznet/pcrecomp")
endif()

if(MSVC)
    add_compile_options(/W1 /wd4996 /wd4244 /wd4146 /wd4102 /wd4101 /wd4267 /wd4018 /wd4305 /bigobj)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
    add_link_options(/STACK:16777216)   # lifted call chains are deep
endif()

add_library(ymfm STATIC
    third_party/ymfm/ymfm_opl.cpp third_party/ymfm/ymfm_adpcm.cpp third_party/ymfm/ymfm_pcm.cpp)
target_include_directories(ymfm PUBLIC third_party/ymfm)

set(RUNTIME src/main.c src/machine.c src/dos.c src/vga.c src/audio.c src/x87.c src/present_d3d.c
            src/host.c src/hires.c src/opl.cpp)

foreach(game wl6)
    set(gen "${CMAKE_SOURCE_DIR}/work/${game}/gen")
    if(EXISTS "${gen}/recomp_dispatch.c")
        file(GLOB lifted CONFIGURE_DEPENDS "${gen}/*.c")
        add_executable(wolf3d_${game} WIN32 ${RUNTIME} ${lifted})
        target_include_directories(wolf3d_${game} PRIVATE src "${gen}" "${PCRECOMP_HOME}/runtime")
        target_link_libraries(wolf3d_${game} PRIVATE ymfm user32 gdi32 winmm d3d11 dxgi dxguid)
        # Optional: NVIDIA DLSS, only when its SDK has been downloaded into
        # external/dlss-sdk (tools/get_dlss.py). The SDK is NVIDIA's, never in
        # this repository; without it the build is the same minus --dlss.
        set(DLSS_SDK "${CMAKE_SOURCE_DIR}/external/dlss-sdk" CACHE PATH "NVIDIA DLSS SDK (tools/get_dlss.py)")
        if(EXISTS "${DLSS_SDK}/include/nvsdk_ngx.h" AND EXISTS "${DLSS_SDK}/lib/nvsdk_ngx_s.lib")
            target_sources(wolf3d_${game} PRIVATE src/dlss.cpp)
            target_include_directories(wolf3d_${game} PRIVATE "${DLSS_SDK}/include")
            target_compile_definitions(wolf3d_${game} PRIVATE WOLF_DLSS)
            target_link_libraries(wolf3d_${game} PRIVATE "${DLSS_SDK}/lib/nvsdk_ngx_s.lib")
            add_custom_command(TARGET wolf3d_${game} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${DLSS_SDK}/bin/nvngx_dlss.dll" "$<TARGET_FILE_DIR:wolf3d_${game}>")
            message(STATUS "wolf3d_${game}: with DLSS (${DLSS_SDK})")
        else()
            message(STATUS "wolf3d_${game}: without DLSS (py tools/get_dlss.py adds it)")
        endif()
        if(WOLF_TRACE)
            target_compile_definitions(wolf3d_${game} PRIVATE RECOMP_TRACE WOLF_DEBUG)
            target_compile_options(wolf3d_${game} PRIVATE /Zi /Oy-)
            target_link_options(wolf3d_${game} PRIVATE /DEBUG)
        endif()
        if(MSVC)
            # a console subsystem exe that still opens a window: stdout stays useful
            set_target_properties(wolf3d_${game} PROPERTIES WIN32_EXECUTABLE OFF)
        endif()
    else()
        message(STATUS "wolf3d_${game}: no lifted code in work/${game}/gen -- run tools/lift.py ${game}")
    endif()
endforeach()

# The hardware models on their own, no game data needed: what CI runs.
add_executable(selftest tests/selftest.c src/vga.c src/x87.c src/audio.c src/opl.cpp)
target_include_directories(selftest PRIVATE src "${PCRECOMP_HOME}/runtime")
target_link_libraries(selftest PRIVATE ymfm)
target_compile_definitions(selftest PRIVATE WOLF_SELFTEST)
