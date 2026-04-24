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

private:
    enum class AppMode
    {
        Edit,
        Play
    };

    std::optional<Window::MessageResult> OnWindowMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    void OnResize(uint32_t width, uint32_t height, bool minimized);
    void Update(float deltaTime);
    void UpdateWindowTitle(float deltaTime);
    bool InitializeImGui();
    void ShutdownImGui();
    void BuildEditorUi();
    void TogglePlayMode(bool enabled);
    void LoadExternalModel();
    void LoadExternalTexture();

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
    float fpsAccumulatedTime_ = 0.0f;
    uint32_t fpsFrameCount_ = 0;
};
} // namespace engine
