cmake_minimum_required(VERSION 3.20)
project(killer7_vr CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "Killer7Win.exe is 32-bit: configure with vcvars32 (build.cmd).")
endif()

# The probe: a stand-in XINPUT1_3.dll that the game loads from its own folder.
add_library(k7vr SHARED
    src/proxy/dllmain.cpp
    src/proxy/log.cpp
    src/proxy/iat.cpp
    src/proxy/vtbl_index.cpp
    src/proxy/ctx_wrap.cpp
    src/proxy/hooks_d3d.cpp
    src/proxy/hooks_compile.cpp
    src/proxy/hooks_folder.cpp
    src/proxy/hooks_input.cpp
    src/proxy/hooks_files.cpp
    src/proxy/audio_mute.cpp
    src/proxy/memscan.cpp
    src/proxy/png.cpp
    src/proxy/control.cpp
    src/proxy/xinput.cpp)
set_target_properties(k7vr PROPERTIES OUTPUT_NAME XINPUT1_3)
target_compile_definitions(k7vr PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
target_compile_options(k7vr PRIVATE /W4 /wd4100 /utf-8)
target_link_libraries(k7vr PRIVATE d3d11 dxgi ole32 windowscodecs uuid)
