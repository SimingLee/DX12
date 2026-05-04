cmake_minimum_required(VERSION 3.27)

project(DX12MiniEngine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(DX12_ENGINE_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
set(DX12_ENGINE_SHADER_SOURCE_DIR ${DX12_ENGINE_ROOT}/shaders)
set(DX12_ENGINE_SHADER_OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/shaders)

file(GLOB DXC_CANDIDATES "C:/Program Files (x86)/Windows Kits/10/bin/*/x64/dxc.exe")
list(SORT DXC_CANDIDATES ORDER DESCENDING)
list(GET DXC_CANDIDATES 0 DXC_EXECUTABLE)

if(NOT EXISTS "${DXC_EXECUTABLE}")
    message(FATAL_ERROR "Unable to find dxc.exe in Windows SDK.")
endif()

set(DX12_ENGINE_LOCAL_ASSIMP_DIR "${DX12_ENGINE_ROOT}/vcpkg_installed/x64-windows/share/assimp")
if(EXISTS "${DX12_ENGINE_LOCAL_ASSIMP_DIR}" AND NOT DEFINED assimp_DIR)
    set(assimp_DIR "${DX12_ENGINE_LOCAL_ASSIMP_DIR}" CACHE PATH "Local vcpkg Assimp package directory")
endif()

find_package(assimp CONFIG QUIET)
if(assimp_FOUND)
    message(STATUS "Assimp found: FBX/advanced mesh import enabled.")
else()
    message(WARNING "Assimp not found. FBX import will be disabled, but OBJ import and Unreal JSON placement still build.")
endif()

set(SHADER_SOURCES
    ${DX12_ENGINE_SHADER_SOURCE_DIR}/common.hlsli
    ${DX12_ENGINE_SHADER_SOURCE_DIR}/forward.hlsl
    ${DX12_ENGINE_SHADER_SOURCE_DIR}/gbuffer.hlsl
    ${DX12_ENGINE_SHADER_SOURCE_DIR}/deferred_lighting.hlsl
    ${DX12_ENGINE_SHADER_SOURCE_DIR}/shadow.hlsl
    ${DX12_ENGINE_SHADER_SOURCE_DIR}/point_shadow.hlsl
)

set(SHADER_OUTPUTS
    ${DX12_ENGINE_SHADER_OUTPUT_DIR}/forward_vs.cso
    ${DX12_ENGINE_SHADER_OUTPUT_DIR}/forward_ps.cso
    ${DX12_ENGINE_SHADER_OUTPUT_DIR}/gbuffer_vs.cso
    ${DX12_ENGINE_SHADER_OUTPUT_DIR}/gbuffer_ps.cso
    ${DX12_ENGINE_SHADER_OUTPUT_DIR}/deferred_lighting_vs.cso
    ${DX12_ENGINE_SHADER_OUTPUT_DIR}/deferred_lighting_ps.cso
    ${DX12_ENGINE_SHADER_OUTPUT_DIR}/shadow_vs.cso
    ${DX12_ENGINE_SHADER_OUTPUT_DIR}/point_shadow_vs.cso
)

file(MAKE_DIRECTORY ${DX12_ENGINE_SHADER_OUTPUT_DIR})

add_custom_command(
    OUTPUT ${DX12_ENGINE_SHADER_OUTPUT_DIR}/forward_vs.cso
    COMMAND "${DXC_EXECUTABLE}" -E VSMain -T vs_6_0 -I ${DX12_ENGINE_SHADER_SOURCE_DIR} -Fo ${DX12_ENGINE_SHADER_OUTPUT_DIR}/forward_vs.cso ${DX12_ENGINE_SHADER_SOURCE_DIR}/forward.hlsl
    DEPENDS ${DX12_ENGINE_SHADER_SOURCE_DIR}/forward.hlsl ${DX12_ENGINE_SHADER_SOURCE_DIR}/common.hlsli
    COMMENT "Compiling vertex shader forward_vs.cso"
    VERBATIM
)

add_custom_command(
    OUTPUT ${DX12_ENGINE_SHADER_OUTPUT_DIR}/forward_ps.cso
    COMMAND "${DXC_EXECUTABLE}" -E PSMain -T ps_6_0 -I ${DX12_ENGINE_SHADER_SOURCE_DIR} -Fo ${DX12_ENGINE_SHADER_OUTPUT_DIR}/forward_ps.cso ${DX12_ENGINE_SHADER_SOURCE_DIR}/forward.hlsl
    DEPENDS ${DX12_ENGINE_SHADER_SOURCE_DIR}/forward.hlsl ${DX12_ENGINE_SHADER_SOURCE_DIR}/common.hlsli
    COMMENT "Compiling pixel shader forward_ps.cso"
    VERBATIM
)

add_custom_command(
    OUTPUT ${DX12_ENGINE_SHADER_OUTPUT_DIR}/gbuffer_vs.cso
    COMMAND "${DXC_EXECUTABLE}" -E VSMain -T vs_6_0 -I ${DX12_ENGINE_SHADER_SOURCE_DIR} -Fo ${DX12_ENGINE_SHADER_OUTPUT_DIR}/gbuffer_vs.cso ${DX12_ENGINE_SHADER_SOURCE_DIR}/gbuffer.hlsl
    DEPENDS ${DX12_ENGINE_SHADER_SOURCE_DIR}/gbuffer.hlsl ${DX12_ENGINE_SHADER_SOURCE_DIR}/common.hlsli
    COMMENT "Compiling vertex shader gbuffer_vs.cso"
    VERBATIM
)

add_custom_command(
    OUTPUT ${DX12_ENGINE_SHADER_OUTPUT_DIR}/gbuffer_ps.cso
    COMMAND "${DXC_EXECUTABLE}" -E PSMain -T ps_6_0 -I ${DX12_ENGINE_SHADER_SOURCE_DIR} -Fo ${DX12_ENGINE_SHADER_OUTPUT_DIR}/gbuffer_ps.cso ${DX12_ENGINE_SHADER_SOURCE_DIR}/gbuffer.hlsl
    DEPENDS ${DX12_ENGINE_SHADER_SOURCE_DIR}/gbuffer.hlsl ${DX12_ENGINE_SHADER_SOURCE_DIR}/common.hlsli
    COMMENT "Compiling pixel shader gbuffer_ps.cso"
    VERBATIM
)

add_custom_command(
    OUTPUT ${DX12_ENGINE_SHADER_OUTPUT_DIR}/deferred_lighting_vs.cso
    COMMAND "${DXC_EXECUTABLE}" -E VSMain -T vs_6_0 -I ${DX12_ENGINE_SHADER_SOURCE_DIR} -Fo ${DX12_ENGINE_SHADER_OUTPUT_DIR}/deferred_lighting_vs.cso ${DX12_ENGINE_SHADER_SOURCE_DIR}/deferred_lighting.hlsl
    DEPENDS ${DX12_ENGINE_SHADER_SOURCE_DIR}/deferred_lighting.hlsl ${DX12_ENGINE_SHADER_SOURCE_DIR}/common.hlsli
    COMMENT "Compiling vertex shader deferred_lighting_vs.cso"
    VERBATIM
)

add_custom_command(
    OUTPUT ${DX12_ENGINE_SHADER_OUTPUT_DIR}/deferred_lighting_ps.cso
    COMMAND "${DXC_EXECUTABLE}" -E PSMain -T ps_6_0 -I ${DX12_ENGINE_SHADER_SOURCE_DIR} -Fo ${DX12_ENGINE_SHADER_OUTPUT_DIR}/deferred_lighting_ps.cso ${DX12_ENGINE_SHADER_SOURCE_DIR}/deferred_lighting.hlsl
    DEPENDS ${DX12_ENGINE_SHADER_SOURCE_DIR}/deferred_lighting.hlsl ${DX12_ENGINE_SHADER_SOURCE_DIR}/common.hlsli
    COMMENT "Compiling pixel shader deferred_lighting_ps.cso"
    VERBATIM
)

add_custom_command(
    OUTPUT ${DX12_ENGINE_SHADER_OUTPUT_DIR}/shadow_vs.cso
    COMMAND "${DXC_EXECUTABLE}" -E VSMain -T vs_6_0 -I ${DX12_ENGINE_SHADER_SOURCE_DIR} -Fo ${DX12_ENGINE_SHADER_OUTPUT_DIR}/shadow_vs.cso ${DX12_ENGINE_SHADER_SOURCE_DIR}/shadow.hlsl
    DEPENDS ${DX12_ENGINE_SHADER_SOURCE_DIR}/shadow.hlsl ${DX12_ENGINE_SHADER_SOURCE_DIR}/common.hlsli
    COMMENT "Compiling vertex shader shadow_vs.cso"
    VERBATIM
)

add_custom_command(
    OUTPUT ${DX12_ENGINE_SHADER_OUTPUT_DIR}/point_shadow_vs.cso
    COMMAND "${DXC_EXECUTABLE}" -E VSMain -T vs_6_0 -I ${DX12_ENGINE_SHADER_SOURCE_DIR} -Fo ${DX12_ENGINE_SHADER_OUTPUT_DIR}/point_shadow_vs.cso ${DX12_ENGINE_SHADER_SOURCE_DIR}/point_shadow.hlsl
    DEPENDS ${DX12_ENGINE_SHADER_SOURCE_DIR}/point_shadow.hlsl ${DX12_ENGINE_SHADER_SOURCE_DIR}/common.hlsli
    COMMENT "Compiling vertex shader point_shadow_vs.cso"
    VERBATIM
)

add_custom_target(Shaders ALL DEPENDS ${SHADER_OUTPUTS})

add_library(imgui STATIC
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_dx12.cpp
    third_party/imgui/backends/imgui_impl_win32.cpp
    third_party/imguizmo/ImGuizmo.cpp
)

target_include_directories(imgui PUBLIC
    ${DX12_ENGINE_ROOT}/third_party/imgui
    ${DX12_ENGINE_ROOT}/third_party/imgui/backends
    ${DX12_ENGINE_ROOT}/third_party/imguizmo
)

target_compile_definitions(imgui PUBLIC
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    IMGUI_IMPL_WIN32_DISABLE_GAMEPAD
)

target_link_libraries(imgui PUBLIC
    d3d12
    dxgi
)

add_executable(DX12MiniEngine WIN32
    src/main.cpp
    src/core/Log.cpp
    src/platform/Window.cpp
    src/engine/Application.cpp
    src/engine/InputSystem.cpp
    src/engine/Camera.cpp
    src/engine/Scene.cpp
    src/renderer/RendererDX12.cpp
)

add_dependencies(DX12MiniEngine Shaders)

target_include_directories(DX12MiniEngine PRIVATE
    ${DX12_ENGINE_ROOT}/src
    ${DX12_ENGINE_ROOT}/third_party/imgui
    ${DX12_ENGINE_ROOT}/third_party/imgui/backends
    ${DX12_ENGINE_ROOT}/third_party/imguizmo
)

target_compile_definitions(DX12MiniEngine PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    ENGINE_SHADER_DIR="${DX12_ENGINE_SHADER_OUTPUT_DIR}"
)

target_compile_options(DX12MiniEngine PRIVATE
    /W4
    /EHsc
    /permissive-
    /Zc:__cplusplus
    /utf-8
)

target_link_libraries(DX12MiniEngine PRIVATE
    comdlg32
    d3d12
    dxgi
    dxguid
    imgui
    ole32
    windowscodecs
)

if(assimp_FOUND)
    target_compile_definitions(DX12MiniEngine PRIVATE ENGINE_HAS_ASSIMP=1)
    target_link_libraries(DX12MiniEngine PRIVATE assimp::assimp)
    set(DX12_ENGINE_VCPKG_RUNTIME_DLL_DIR
        "$<IF:$<CONFIG:Debug>,${DX12_ENGINE_ROOT}/vcpkg_installed/x64-windows/debug/bin,${DX12_ENGINE_ROOT}/vcpkg_installed/x64-windows/bin>")
    add_custom_command(TARGET DX12MiniEngine POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:DX12MiniEngine>
            $<TARGET_FILE_DIR:DX12MiniEngine>
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${DX12_ENGINE_VCPKG_RUNTIME_DLL_DIR}
            $<TARGET_FILE_DIR:DX12MiniEngine>
        COMMAND_EXPAND_LISTS
        COMMENT "Copying runtime DLL dependencies"
    )
else()
    target_compile_definitions(DX12MiniEngine PRIVATE ENGINE_HAS_ASSIMP=0)
endif()

set_target_properties(DX12MiniEngine PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)
