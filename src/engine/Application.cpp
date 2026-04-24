#include "engine/Application.h"

#include "core/Log.h"
#include "core/Timer.h"

#include "imgui.h"
#include "backends/imgui_impl_win32.h"

#include <CommDlg.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <string>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

namespace engine
{
namespace
{
constexpr float kEditorPanelWidth = 430.0f;
constexpr float kEditorPanelHeight = 270.0f;
constexpr float kEditorPanelMargin = 16.0f;

std::wstring OpenFileDialog(HWND owner, const wchar_t* filter)
{
    std::array<wchar_t, MAX_PATH> buffer{};

    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = owner;
    dialog.lpstrFilter = filter;
    dialog.lpstrFile = buffer.data();
    dialog.nMaxFile = static_cast<DWORD>(buffer.size());
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;

    if (::GetOpenFileNameW(&dialog) == FALSE)
    {
        return {};
    }

    return buffer.data();
}

std::string Narrow(const std::wstring& value)
{
    if (value.empty())
    {
        return {};
    }

    const int size = ::WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(size), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}
} // namespace

bool Application::Initialize()
{
    InitializeLogging();
    Log(LogLevel::Info, "Starting DX12 mini engine.");

    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    if (!window_.Create(baseWindowTitle_.c_str(), 1280, 720))
    {
        return false;
    }

    window_.SetMessageCallback([this](HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        return OnWindowMessage(hwnd, message, wParam, lParam);
    });
    window_.SetResizeCallback([this](uint32_t width, uint32_t height, bool minimized) {
        OnResize(width, height, minimized);
    });

    if (!renderer_.Initialize(window_))
    {
        return false;
    }
    if (!InitializeImGui())
    {
        return false;
    }

    initialized_ = true;
    return true;
}

int Application::Run()
{
    if (!initialized_)
    {
        return -1;
    }

    Timer timer;
    while (true)
    {
        input_.BeginFrame();
        if (!window_.PumpMessages())
        {
            break;
        }

        const bool buildEditorUi = imguiInitialized_ && mode_ == AppMode::Edit;
        if (buildEditorUi)
        {
            renderer_.BeginImGuiFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();
            BuildEditorUi();
        }

        const float deltaTime = timer.Tick();
        Update(deltaTime);

        ImDrawData* drawData = nullptr;
        if (buildEditorUi)
        {
            ImGui::Render();
            if (mode_ == AppMode::Edit)
            {
                drawData = ImGui::GetDrawData();
            }
        }

        if (!minimized_)
        {
            renderer_.Render(scene_, camera_, drawData);
        }
    }

    Shutdown();
    return 0;
}

void Application::Shutdown()
{
    if (!initialized_)
    {
        return;
    }

    input_.ReleaseMouseCapture(window_.Handle());
    renderer_.Shutdown();
    ShutdownImGui();
    initialized_ = false;
    ::CoUninitialize();
    Log(LogLevel::Info, "DX12 mini engine shut down cleanly.");
}

std::optional<Window::MessageResult> Application::OnWindowMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    bool imguiHandled = false;
    if (imguiInitialized_)
    {
        imguiHandled = ImGui_ImplWin32_WndProcHandler(hwnd, message, wParam, lParam) != 0;
    }

    input_.HandleMessage(hwnd, message, wParam, lParam);

    if (imguiHandled)
    {
        return Window::MessageResult{true, 0};
    }

    return std::nullopt;
}

void Application::OnResize(uint32_t width, uint32_t height, bool minimized)
{
    minimized_ = minimized || width == 0 || height == 0;
    std::ostringstream stream;
    stream << "Window resize event: " << width << "x" << height;
    if (minimized_)
    {
        stream << " (minimized)";
    }
    Log(LogLevel::Info, stream.str());

    if (!minimized_)
    {
        renderer_.Resize(width, height);
    }
}

void Application::Update(float deltaTime)
{
    if (input_.WasKeyPressed(VK_ESCAPE))
    {
        if (mode_ == AppMode::Play)
        {
            TogglePlayMode(false);
            return;
        }

        if (input_.IsMouseCaptured())
        {
            input_.ReleaseMouseCapture(window_.Handle());
        }
        else
        {
            window_.RequestClose();
            return;
        }
    }

    if (mode_ == AppMode::Edit)
    {
        bool uiWantsKeyboard = false;
        bool uiWantsMouse = false;
        if (imguiInitialized_)
        {
            const ImGuiIO& io = ImGui::GetIO();
            uiWantsKeyboard = io.WantCaptureKeyboard || io.WantTextInput;
            uiWantsMouse = io.WantCaptureMouse;
        }

        if (!uiWantsKeyboard && !uiWantsMouse)
        {
            camera_.Update(input_, deltaTime);
        }
        scene_.Update(deltaTime);
    }
    else
    {
        scene_.UpdatePlayer(input_, deltaTime);
        const DirectX::XMFLOAT3 playerPosition = scene_.PlayerPosition();
        const float playerYaw = scene_.PlayerYaw();
        const float followDistance = 4.5f;
        const float followHeight = 2.2f;
        const float forwardX = std::sinf(playerYaw);
        const float forwardZ = std::cosf(playerYaw);
        const DirectX::XMFLOAT3 cameraPosition{
            playerPosition.x - forwardX * followDistance,
            playerPosition.y + followHeight,
            playerPosition.z - forwardZ * followDistance};
        camera_.SetTransform(cameraPosition, playerYaw, 0.22f);
    }

    UpdateWindowTitle(deltaTime);
}

void Application::UpdateWindowTitle(float deltaTime)
{
    fpsAccumulatedTime_ += deltaTime;
    ++fpsFrameCount_;

    if (fpsAccumulatedTime_ < 0.25f)
    {
        return;
    }

    const float fps = static_cast<float>(fpsFrameCount_) / fpsAccumulatedTime_;
    const float frameTimeMs = fps > 0.0f ? 1000.0f / fps : 0.0f;
    const wchar_t* modeText = mode_ == AppMode::Edit ? L"Edit" : L"Play";

    std::wostringstream stream;
    stream << baseWindowTitle_
           << L" | Mode: " << modeText
           << L" | FPS: " << std::fixed << std::setprecision(1) << fps
           << L" | Frame: " << std::fixed << std::setprecision(2) << frameTimeMs << L" ms";
    if (mode_ == AppMode::Play)
    {
        stream << L" | Esc: Back to Edit";
    }
    window_.SetTitle(stream.str());

    fpsAccumulatedTime_ = 0.0f;
    fpsFrameCount_ = 0;
}

bool Application::InitializeImGui()
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();

    if (!ImGui_ImplWin32_Init(window_.Handle()))
    {
        Log(LogLevel::Error, "Failed to initialize ImGui Win32 backend.");
        return false;
    }
    if (!renderer_.InitializeImGui())
    {
        ImGui_ImplWin32_Shutdown();
        return false;
    }

    imguiInitialized_ = true;
    Log(LogLevel::Info, "ImGui initialized successfully.");
    return true;
}

void Application::ShutdownImGui()
{
    if (!imguiInitialized_)
    {
        return;
    }

    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    imguiInitialized_ = false;
}

void Application::BuildEditorUi()
{
    ImGui::SetNextWindowPos(ImVec2(kEditorPanelMargin, kEditorPanelMargin), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(kEditorPanelWidth, kEditorPanelHeight), ImGuiCond_Always);

    constexpr ImGuiWindowFlags windowFlags =
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize;

    if (!ImGui::Begin("Editor Tools", nullptr, windowFlags))
    {
        ImGui::End();
        return;
    }

    if (ImGui::Button("Load Model", ImVec2(120.0f, 0.0f)))
    {
        LoadExternalModel();
    }
    ImGui::SameLine();
    if (ImGui::Button("Load Texture", ImVec2(120.0f, 0.0f)))
    {
        LoadExternalTexture();
    }
    ImGui::SameLine();
    bool playMode = false;
    if (ImGui::Checkbox("Play Mode", &playMode) && playMode)
    {
        TogglePlayMode(true);
    }

    ImGui::Separator();
    ImGui::TextUnformatted(mode_ == AppMode::Edit ? "Mode: Edit (free camera)" : "Mode: Play (follow object)");
    ImGui::TextWrapped("%s", Narrow(loadedModelLabel_).c_str());
    ImGui::TextWrapped("%s", Narrow(loadedTextureLabel_).c_str());

    if (ImGui::CollapsingHeader("Scene", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::TextUnformatted("Scene outliner and object list will live here.");
    }
    if (ImGui::CollapsingHeader("Selection", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::TextUnformatted("Selection state and transform controls are reserved for the next phase.");
    }
    if (ImGui::CollapsingHeader("Inspector", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::TextUnformatted("Material, transform, and gameplay properties will be edited here.");
    }

    ImGui::End();
}

void Application::TogglePlayMode(bool enabled)
{
    if (enabled && mode_ == AppMode::Play)
    {
        return;
    }
    if (!enabled && mode_ == AppMode::Edit)
    {
        return;
    }

    if (enabled)
    {
        savedEditCameraPosition_ = camera_.Position();
        savedEditCameraYaw_ = camera_.Yaw();
        savedEditCameraPitch_ = camera_.Pitch();
        ::SetFocus(window_.Handle());
        input_.CaptureMouse(window_.Handle(), true);
        mode_ = AppMode::Play;
        Log(LogLevel::Info, "Switched to play mode.");
    }
    else
    {
        input_.ReleaseMouseCapture(window_.Handle());
        camera_.SetTransform(savedEditCameraPosition_, savedEditCameraYaw_, savedEditCameraPitch_);
        mode_ = AppMode::Edit;
        ::SetFocus(window_.Handle());
        Log(LogLevel::Info, "Switched to edit mode.");
    }
}

void Application::LoadExternalModel()
{
    const std::wstring path = OpenFileDialog(window_.Handle(), L"OBJ Files\0*.obj\0All Files\0*.*\0\0");
    if (path.empty())
    {
        return;
    }

    if (renderer_.LoadModelFromFile(path))
    {
        loadedModelLabel_ = L"Model: " + path;
        Log(LogLevel::Info, "Loaded external model.");
    }
}

void Application::LoadExternalTexture()
{
    const std::wstring path = OpenFileDialog(
        window_.Handle(),
        L"Image Files\0*.png;*.jpg;*.jpeg;*.bmp;*.tif;*.tiff\0All Files\0*.*\0\0");
    if (path.empty())
    {
        return;
    }

    if (renderer_.LoadTextureFromFile(path))
    {
        loadedTextureLabel_ = L"Texture: " + path;
        Log(LogLevel::Info, "Loaded external texture.");
    }
}
} // namespace engine
