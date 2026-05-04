#pragma once

#include "engine/Camera.h"
#include "engine/InputSystem.h"
#include "engine/Scene.h"
#include "platform/Window.h"
#include "renderer/RendererDX12.h"

#include <Windows.h>

#include <optional>
#include <string>

namespace engine
{
class Application
{
public:
    bool Initialize();
    int Run();
    void Shutdown();

    enum class AppMode
    {
        Edit,
        Play
    };

    enum class GizmoOperation
    {
        Translate,
        Rotate,
        Scale
    };

    enum class GizmoMode
    {
        Local,
        World
    };

private:
    std::optional<Window::MessageResult> OnWindowMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    void OnResize(uint32_t width, uint32_t height, bool minimized);
    void Update(float deltaTime);
    void UpdateWindowTitle(float deltaTime);
    bool InitializeImGui();
    void ShutdownImGui();
    void BuildEditorUi();
    void BuildScenePanel();
    void BuildInspectorPanel();
    void UpdateEditorPicking();
    void DrawTransformGizmo();
    void DrawLightGizmos();
    void TogglePlayMode(bool enabled);
    void LoadExternalModel();
    void LoadExternalTexture();
    void ImportUnrealLevelJson();

    Window window_{};
    InputSystem input_{};
    Camera camera_{};
    Scene scene_{};
    RendererDX12 renderer_{};
    bool minimized_ = false;
    bool initialized_ = false;
    bool imguiInitialized_ = false;
    AppMode mode_ = AppMode::Edit;
    std::wstring baseWindowTitle_ = L"DX12 Mini Engine";
    std::wstring loadedModelLabel_ = L"Model: default cube";
    std::wstring loadedTextureLabel_ = L"Texture: generated checkerboard";
    DirectX::XMFLOAT3 savedEditCameraPosition_{};
    float savedEditCameraYaw_ = 0.0f;
    float savedEditCameraPitch_ = 0.0f;
    float playCameraYaw_ = 0.0f;
    float playCameraPitch_ = 0.22f;
    GizmoOperation gizmoOperation_ = GizmoOperation::Translate;
    GizmoMode gizmoMode_ = GizmoMode::Local;
    float fpsAccumulatedTime_ = 0.0f;
    uint32_t fpsFrameCount_ = 0;
};
} // namespace engine
