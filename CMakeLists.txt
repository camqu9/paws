cmake_minimum_required(VERSION 3.20)
project(paws VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Place all executables and .asi plugins in the same output directory
# so paws.exe can auto-deploy paws_gtav.asi into GTA5.exe's directory.
set(PAWS_OUTPUT_DIR "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PAWS_OUTPUT_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${PAWS_OUTPUT_DIR}")
if(CMAKE_CONFIGURATION_TYPES)
    foreach(CFG Debug Release RelWithDebInfo MinSizeRel)
        string(TOUPPER "${CFG}" CFG_UPPER)
        set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${CFG_UPPER} "${PAWS_OUTPUT_DIR}/${CFG}")
        set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${CFG_UPPER} "${PAWS_OUTPUT_DIR}/${CFG}")
    endforeach()
endif()

# ============================================================================
# Windows: Universal FSR 1.0 Wrapper (paws.exe) + GTA V ASI Plugin (paws_gtav.asi)
# ============================================================================
if(WIN32)
    # 1. Universal FSR 1.0 Wrapper & GTA V Smart Launcher (paws.exe)
    add_executable(paws
        src/windows/main.cpp
    )
    target_link_libraries(paws PRIVATE
        d3d11
        dxgi
        d3dcompiler
        user32
        gdi32
        dwmapi
    )
    if(MSVC)
        target_compile_options(paws PRIVATE /W4 /permissive- /utf-8)
    endif()

    # 2. Embed HLSL compute shaders into generated headers
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/asi-mod/shaders/motion_vectors.hlsl" PAWS_MV_HLSL_CONTENT)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/asi-mod/shaders/temporal_upscale.hlsl" PAWS_TU_HLSL_CONTENT)
    set(PAWS_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
    file(MAKE_DIRECTORY "${PAWS_GENERATED_DIR}")
    file(WRITE "${PAWS_GENERATED_DIR}/motion_vectors_shader.h"
        "#pragma once\n"
        "namespace paws {\n"
        "static constexpr const char* kMotionVectorsHlsl = R\"PAWS_HLSL(\n"
        "${PAWS_MV_HLSL_CONTENT}\n"
        ")PAWS_HLSL\";\n"
        "} // namespace paws\n"
    )
    file(WRITE "${PAWS_GENERATED_DIR}/temporal_upscale_shader.h"
        "#pragma once\n"
        "namespace paws {\n"
        "static constexpr const char* kTemporalUpscaleHlsl = R\"PAWS_HLSL(\n"
        "${PAWS_TU_HLSL_CONTENT}\n"
        ")PAWS_HLSL\";\n"
        "} // namespace paws\n"
    )

    # 3. GTA V Legacy DirectX 11 ASI Plugin (paws_gtav.asi)
    add_library(paws_gtav SHARED
        asi-mod/src/dllmain.cpp
        asi-mod/src/dx11_hook.cpp
        asi-mod/src/camera.cpp
        asi-mod/src/fsr_pipeline.cpp
    )
    set_target_properties(paws_gtav PROPERTIES
        PREFIX ""
        SUFFIX ".asi"
    )
    target_include_directories(paws_gtav PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/asi-mod/include"
        "${PAWS_GENERATED_DIR}"
    )
    target_link_libraries(paws_gtav PRIVATE
        d3d11
        dxgi
        d3dcompiler
        user32
    )
    if(MSVC)
        target_compile_options(paws_gtav PRIVATE /W4 /permissive- /utf-8)
    endif()

    # Optional AMD FidelityFX FSR 2/3 SDK integration
    set(PAWS_FFX_SDK_DIR "" CACHE PATH "Path to AMD FidelityFX FSR 2/3 SDK root directory")
    if(PAWS_FFX_SDK_DIR AND EXISTS "${PAWS_FFX_SDK_DIR}/sdk/include/FidelityFX/host/ffx_fsr2.h")
        message(STATUS "paws_gtav: AMD FidelityFX SDK found at ${PAWS_FFX_SDK_DIR} (enabling HAS_FFX_SDK)")
        target_compile_definitions(paws_gtav PRIVATE HAS_FFX_SDK=1)
        target_include_directories(paws_gtav PRIVATE "${PAWS_FFX_SDK_DIR}/sdk/include")
        target_link_directories(paws_gtav PRIVATE "${PAWS_FFX_SDK_DIR}/sdk/bin/ffx_sdk")
        target_link_libraries(paws_gtav PRIVATE ffx_fsr2_x64 ffx_backend_dx11_x64)
    else()
        message(STATUS "paws_gtav: Building without AMD FidelityFX SDK (HAS_FFX_SDK not defined). "
                       "Pass -DPAWS_FFX_SDK_DIR=<path> to enable FSR 2/3 dispatch.")
    endif()

# ============================================================================
# Linux: Universal FSR 1.0 Wrapper (paws)
# ============================================================================
elseif(UNIX AND NOT APPLE)
    find_package(X11 REQUIRED)
    find_package(OpenGL REQUIRED)
    find_library(XCOMPOSITE_LIB Xcomposite REQUIRED)
    find_library(XFIXES_LIB Xfixes REQUIRED)

    add_executable(paws
        src/linux/main.cpp
    )
    target_include_directories(paws PRIVATE
        ${X11_INCLUDE_DIR}
        ${OPENGL_INCLUDE_DIR}
    )
    target_link_libraries(paws PRIVATE
        ${X11_LIBRARIES}
        ${XCOMPOSITE_LIB}
        ${XFIXES_LIB}
        ${OPENGL_LIBRARIES}
    )
    target_compile_options(paws PRIVATE -Wall -Wextra -Wpedantic)
endif()
