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

set(SHADER_SOURCES
    ${DX12_ENGINE_SHADER_SOURCE_DIR}/basic.hlsl
)

set(SHADER_OUTPUTS
    ${DX12_ENGINE_SHADER_OUTPUT_DIR}/basic_vs.cso
    ${DX12_ENGINE_SHADER_OUTPUT_DIR}/basic_ps.cso
)

file(MAKE_DIRECTORY ${DX12_ENGINE_SHADER_OUTPUT_DIR})

add_custom_command(
    OUTPUT ${DX12_ENGINE_SHADER_OUTPUT_DIR}/basic_vs.cso
    COMMAND "${DXC_EXECUTABLE}" -E VSMain -T vs_6_0 -Fo ${DX12_ENGINE_SHADER_OUTPUT_DIR}/basic_vs.cso ${DX12_ENGINE_SHADER_SOURCE_DIR}/basic.hlsl
    DEPENDS ${DX12_ENGINE_SHADER_SOURCE_DIR}/basic.hlsl
    COMMENT "Compiling vertex shader basic_vs.cso"
    VERBATIM
)

add_custom_command(
    OUTPUT ${DX12_ENGINE_SHADER_OUTPUT_DIR}/basic_ps.cso
    COMMAND "${DXC_EXECUTABLE}" -E PSMain -T ps_6_0 -Fo ${DX12_ENGINE_SHADER_OUTPUT_DIR}/basic_ps.cso ${DX12_ENGINE_SHADER_SOURCE_DIR}/basic.hlsl
    DEPENDS ${DX12_ENGINE_SHADER_SOURCE_DIR}/basic.hlsl
    COMMENT "Compiling pixel shader basic_ps.cso"
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
)

target_include_directories(imgui PUBLIC
    ${DX12_ENGINE_ROOT}/third_party/imgui
    ${DX12_ENGINE_ROOT}/third_party/imgui/backends
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

set_target_properties(DX12MiniEngine PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)
