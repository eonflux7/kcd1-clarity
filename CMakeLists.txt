# KCD1 Clarity: DLSS for Kingdom Come: Deliverance. Built on Windows with MSVC (build.cmd).
# Output is dinput8.dll: a proxy for System32\dinput8.dll that WHGame.dll loads from Bin\Win64.
cmake_minimum_required(VERSION 3.24)
project(kcd_clarity VERSION 0.2.0 LANGUAGES C CXX ASM_MASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: nothing extra to install next to the game.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_POLICY_DEFAULT_CMP0135 NEW)

include(FetchContent)
# SOURCE_SUBDIR points at a missing dir so FetchContent only downloads; targets are defined below.
FetchContent_Declare(imgui
    URL https://github.com/ocornut/imgui/archive/refs/tags/v1.91.5.tar.gz
    URL_HASH SHA256=2aa2d169c569368439e5d5667e0796d09ca5cc6432965ce082e516937d7db254
    SOURCE_SUBDIR _no_cmake)
FetchContent_Declare(minhook
    URL https://github.com/TsudaKageyu/minhook/archive/refs/tags/v1.3.3.tar.gz
    URL_HASH SHA256=5bec16358ec9086d4593124bf558635e89135abea2c76e5761ecaf09f4546b19
    SOURCE_SUBDIR _no_cmake)
FetchContent_MakeAvailable(imgui minhook)

# NVIDIA DLSS SDK (NGX): only the x64 files we need, straight from the tagged repo. Not committed and
# not redistributed on its own (LICENSE.txt in the SDK); nvngx_dlss.dll is deployed next to the game.
set(DLSS_SDK_TAG v310.9.1)
set(DLSS_SDK_DIR ${CMAKE_BINARY_DIR}/dlss-sdk-${DLSS_SDK_TAG})
# cmake/dlss-sdk-<tag>.sha256 lists every file we take from the SDK and its hash ("<sha256>  <path>" lines).
file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/cmake/dlss-sdk-${DLSS_SDK_TAG}.sha256 DLSS_SDK_MANIFEST)
foreach(line ${DLSS_SDK_MANIFEST})
    string(REGEX MATCH "^([0-9a-f]+)  (.+)$" _ "${line}")
    set(hash ${CMAKE_MATCH_1})
    set(f ${CMAKE_MATCH_2})
    if(NOT EXISTS ${DLSS_SDK_DIR}/${f})
        message(STATUS "DLSS SDK: downloading ${f}")
        file(DOWNLOAD https://raw.githubusercontent.com/NVIDIA/DLSS/${DLSS_SDK_TAG}/${f} ${DLSS_SDK_DIR}/${f}.part
             STATUS st TLS_VERIFY ON EXPECTED_HASH SHA256=${hash})
        list(GET st 0 code)
        if(NOT code EQUAL 0)
            file(REMOVE ${DLSS_SDK_DIR}/${f}.part)
            message(FATAL_ERROR "DLSS SDK download failed for ${f}: ${st}")
        endif()
        file(RENAME ${DLSS_SDK_DIR}/${f}.part ${DLSS_SDK_DIR}/${f})
    endif()
endforeach()
# Copied next to dinput8.dll: rel ships in the release zip, dev has the DLSS debug overlay.
file(COPY ${DLSS_SDK_DIR}/lib/Windows_x86_64/rel/nvngx_dlss.dll DESTINATION ${CMAKE_BINARY_DIR}/ngx/rel)
file(COPY ${DLSS_SDK_DIR}/lib/Windows_x86_64/dev/nvngx_dlss.dll DESTINATION ${CMAKE_BINARY_DIR}/ngx/dev)

file(GLOB_RECURSE MINHOOK_SOURCES ${minhook_SOURCE_DIR}/src/*.c)
add_library(minhook STATIC ${MINHOOK_SOURCES})
target_include_directories(minhook PUBLIC ${minhook_SOURCE_DIR}/include)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_compile_definitions(imgui PUBLIC IMGUI_IMPL_WIN32_DISABLE_GAMEPAD)

add_library(kcd_clarity SHARED
    src/core/dllmain.cpp
    src/core/log.cpp
    src/core/config.cpp
    src/core/crash.cpp
    src/core/d3d_debug.cpp
    src/core/proxy_dinput8.cpp
    src/core/proxy_dinput8.asm
    src/core/dinput8.def
    src/game/renderer.cpp
    src/game/symbols.cpp
    src/hooks/d3d11_hooks.cpp
    src/hooks/present.cpp
    src/renderer/dxbc_patch.cpp
    src/renderer/subrect_fixes.cpp
    src/renderer/canary.cpp
    src/renderer/frame_dump.cpp
    src/renderer/mip_bias.cpp
    src/renderer/shadow_noise.cpp
    src/renderer/velocity_format.cpp
    src/renderer/velocity_mask.cpp
    src/renderer/screen_size_fix.cpp
    src/renderer/state_backup.cpp
    src/renderer/taa_dlss.cpp
    src/upscaler/dlss.cpp
    src/ui/input.cpp
    src/ui/overlay.cpp)
set_target_properties(kcd_clarity PROPERTIES OUTPUT_NAME dinput8)
target_include_directories(kcd_clarity PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${DLSS_SDK_DIR}/include)
target_compile_definitions(kcd_clarity PRIVATE
    KC_VERSION="${PROJECT_VERSION}" WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(kcd_clarity PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:/W4;/permissive->")
# No static import of d3d11/dxgi: the loader waits for the game to load d3d11.dll itself.
# d3dcompiler (ImGui DX11 backend, depth-copy shader) is delay-loaded so the proxy pulls in nothing at process start.
target_link_libraries(kcd_clarity PRIVATE minhook imgui delayimp d3dcompiler
    ${DLSS_SDK_DIR}/lib/Windows_x86_64/x64/nvsdk_ngx_s.lib)
target_link_options(kcd_clarity PRIVATE /DELAYLOAD:d3dcompiler_47.dll /DELAYLOAD:dwmapi.dll /IGNORE:4199
    /PDBALTPATH:%_PDB%)  # only the PDB file name in the DLL, not the build path
