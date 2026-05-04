#include "engine/Application.h"

#include "core/Log.h"
#include "core/Timer.h"

#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "ImGuizmo.h"

#include <CommDlg.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

namespace engine
{
namespace
{
constexpr float kEditorPanelWidth = 430.0f;
constexpr float kEditorPanelHeight = 620.0f;
constexpr float kEditorPanelMargin = 16.0f;

const char* MeshTypeName(MeshType type)
{
    switch (type)
    {
    case MeshType::Cube:
        return "Cube";
    case MeshType::Sphere:
        return "Sphere";
    case MeshType::ExternalModel:
        return "External Model";
    default:
        return "Unknown";
    }
}

const char* LightTypeName(LightType type)
{
    switch (type)
    {
    case LightType::Directional:
        return "Directional Light";
    case LightType::Point:
        return "Point Light";
    default:
        return "Unknown Light";
    }
}

const char* BodyTypeName(BodyType type)
{
    switch (type)
    {
    case BodyType::Dynamic:
        return "Dynamic";
    case BodyType::Static:
    default:
        return "Static";
    }
}

const char* ColliderTypeName(ColliderType type)
{
    switch (type)
    {
    case ColliderType::Sphere:
        return "Sphere";
    case ColliderType::Box:
    default:
        return "Box";
    }
}

const char* SceneObjectKindName(const SceneObject& object)
{
    if (object.HasLight())
    {
        return LightTypeName(object.light->type);
    }
    if (object.HasMesh())
    {
        return MeshTypeName(object.mesh->fallbackMeshType);
    }
    return "Object";
}

const char* RenderPipelineModeName(RenderPipelineMode mode)
{
    switch (mode)
    {
    case RenderPipelineMode::Deferred:
        return "Deferred";
    case RenderPipelineMode::Forward:
    default:
        return "Forward";
    }
}

ImGuizmo::OPERATION ToImGuizmoOperation(Application::GizmoOperation operation)
{
    switch (operation)
    {
    case Application::GizmoOperation::Rotate:
        return ImGuizmo::ROTATE;
    case Application::GizmoOperation::Scale:
        return ImGuizmo::SCALE;
    case Application::GizmoOperation::Translate:
    default:
        return ImGuizmo::TRANSLATE;
    }
}

ImGuizmo::MODE ToImGuizmoMode(Application::GizmoMode mode)
{
    return mode == Application::GizmoMode::World ? ImGuizmo::WORLD : ImGuizmo::LOCAL;
}

DirectX::XMFLOAT3 LightDirectionFromTransform(const Transform& transform)
{
    const DirectX::XMMATRIX rotation = DirectX::XMMatrixRotationRollPitchYaw(
        transform.rotationEuler.x,
        transform.rotationEuler.y,
        transform.rotationEuler.z);
    const DirectX::XMVECTOR direction = DirectX::XMVector3Normalize(
        DirectX::XMVector3TransformNormal(DirectX::XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f), rotation));
    DirectX::XMFLOAT3 result{};
    DirectX::XMStoreFloat3(&result, direction);
    return result;
}

bool ProjectWorldPoint(
    const DirectX::XMFLOAT3& worldPoint,
    const DirectX::XMMATRIX& viewProjection,
    float width,
    float height,
    ImVec2& screenPoint)
{
    const DirectX::XMVECTOR projected = DirectX::XMVector3TransformCoord(DirectX::XMLoadFloat3(&worldPoint), viewProjection);
    DirectX::XMFLOAT3 ndc{};
    DirectX::XMStoreFloat3(&ndc, projected);
    if (ndc.z < 0.0f || ndc.z > 1.0f)
    {
        return false;
    }

    screenPoint.x = (ndc.x * 0.5f + 0.5f) * width;
    screenPoint.y = (-ndc.y * 0.5f + 0.5f) * height;
    return true;
}

float NormalizeAngle(float angle)
{
    while (angle > DirectX::XM_PI)
    {
        angle -= DirectX::XM_2PI;
    }
    while (angle < -DirectX::XM_PI)
    {
        angle += DirectX::XM_2PI;
    }
    return angle;
}

float ClampAngleAround(float angle, float center, float maxDelta)
{
    const float delta = std::clamp(NormalizeAngle(angle - center), -maxDelta, maxDelta);
    return center + delta;
}

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

std::wstring WidenUtf8(const std::string& value)
{
    if (value.empty())
    {
        return {};
    }

    const int size = ::MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(static_cast<size_t>(size), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), result.data(), size);
    return result;
}

std::string UnescapeJsonString(const std::string& value)
{
    std::string result;
    result.reserve(value.size());
    bool escaping = false;
    for (char c : value)
    {
        if (!escaping)
        {
            if (c == '\\')
            {
                escaping = true;
            }
            else
            {
                result.push_back(c);
            }
            continue;
        }

        switch (c)
        {
        case '\\':
        case '"':
        case '/':
            result.push_back(c);
            break;
        case 'n':
            result.push_back('\n');
            break;
        case 't':
            result.push_back('\t');
            break;
        default:
            result.push_back(c);
            break;
        }
        escaping = false;
    }
    return result;
}

bool ExtractJsonString(const std::string& objectText, const char* fieldName, std::string& outValue)
{
    const std::regex pattern(std::string("\"") + fieldName + R"json("\s*:\s*"((?:\\.|[^"\\])*)")json");
    std::smatch match;
    if (!std::regex_search(objectText, match, pattern))
    {
        return false;
    }
    outValue = UnescapeJsonString(match[1].str());
    return true;
}

bool ExtractJsonVec3(const std::string& objectText, const char* fieldName, DirectX::XMFLOAT3& outValue)
{
    const std::regex arrayPattern(
        std::string("\"") + fieldName +
        R"("\s*:\s*\[\s*([-+0-9.eE]+)\s*,\s*([-+0-9.eE]+)\s*,\s*([-+0-9.eE]+)\s*\])");
    std::smatch match;
    if (!std::regex_search(objectText, match, arrayPattern))
    {
        return false;
    }

    outValue = {
        std::stof(match[1].str()),
        std::stof(match[2].str()),
        std::stof(match[3].str())};
    return true;
}

std::string FileStemUtf8(const std::wstring& path)
{
    const std::wstring stem = std::filesystem::path(path).stem().wstring();
    return stem.empty() ? "Imported Mesh" : Narrow(stem);
}

DirectX::XMFLOAT3 UnrealLocationToEngine(const DirectX::XMFLOAT3& locationCentimeters)
{
    return {
        locationCentimeters.x * 0.01f,
        locationCentimeters.z * 0.01f,
        locationCentimeters.y * 0.01f};
}

DirectX::XMFLOAT3 UnrealRotationToEngineRadians(const DirectX::XMFLOAT3& rotationDegrees)
{
    return {
        DirectX::XMConvertToRadians(rotationDegrees.x),
        DirectX::XMConvertToRadians(rotationDegrees.z),
        DirectX::XMConvertToRadians(rotationDegrees.y)};
}

DirectX::XMFLOAT3 UnrealScaleToEngine(const DirectX::XMFLOAT3& scale)
{
    return {scale.x, scale.z, scale.y};
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
            ImGuizmo::BeginFrame();
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
        UpdateEditorPicking();
        scene_.Update(deltaTime);
    }
    else
    {
        scene_.UpdatePlayPhysics(input_, deltaTime);
        const MouseDelta mouseDelta = input_.GetMouseDelta();
        constexpr float lookSpeed = 0.0025f;
        constexpr float maxCameraYawOffset = DirectX::XMConvertToRadians(45.0f);
        const float playerYaw = scene_.PlayerYaw();
        playCameraYaw_ += mouseDelta.x * lookSpeed;
        playCameraYaw_ = ClampAngleAround(playCameraYaw_, playerYaw, maxCameraYawOffset);
        playCameraPitch_ += mouseDelta.y * lookSpeed;
        playCameraPitch_ = std::clamp(playCameraPitch_, -0.15f, 0.85f);

        const DirectX::XMFLOAT3 playerPosition = scene_.PlayerPosition();
        const float followDistance = 4.5f;
        const float followHeight = 2.2f;
        const float forwardX = std::sinf(playCameraYaw_) * std::cosf(playCameraPitch_);
        const float forwardY = -std::sinf(playCameraPitch_);
        const float forwardZ = std::cosf(playCameraYaw_) * std::cosf(playCameraPitch_);
        const DirectX::XMFLOAT3 cameraPosition{
            playerPosition.x - forwardX * followDistance,
            playerPosition.y + followHeight - forwardY * followDistance,
            playerPosition.z - forwardZ * followDistance};
        camera_.SetTransform(cameraPosition, playCameraYaw_, playCameraPitch_);
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
    if (ImGui::Button("Import Unreal Level", ImVec2(170.0f, 0.0f)))
    {
        ImportUnrealLevelJson();
    }

    ImGui::Separator();
    ImGui::TextUnformatted(mode_ == AppMode::Edit ? "Mode: Edit (free camera)" : "Mode: Play (follow object)");
    ImGui::TextWrapped("%s", Narrow(loadedModelLabel_).c_str());
    ImGui::TextWrapped("%s", Narrow(loadedTextureLabel_).c_str());

    if (ImGui::CollapsingHeader("Rendering", ImGuiTreeNodeFlags_DefaultOpen))
    {
        RenderPipelineMode currentMode = renderer_.Mode();
        int selectedMode = static_cast<int>(currentMode);
        const char* modes[] = {"Forward", "Deferred"};
        if (ImGui::Combo("Pipeline", &selectedMode, modes, static_cast<int>(std::size(modes))))
        {
            renderer_.SetRenderPipelineMode(static_cast<RenderPipelineMode>(selectedMode));
        }
        ImGui::Text("Active: %s", RenderPipelineModeName(renderer_.Mode()));
    }

    BuildScenePanel();
    if (ImGui::CollapsingHeader("Selection", ImGuiTreeNodeFlags_DefaultOpen))
    {
        if (const std::optional<uint32_t> selectedId = scene_.SelectedObjectId())
        {
            const SceneObject* selectedObject = scene_.FindObject(*selectedId);
            ImGui::Text("Selected: %s", selectedObject != nullptr ? selectedObject->name.c_str() : "None");
        }
        else
        {
            ImGui::TextUnformatted("Selected: None");
        }
    }
    BuildInspectorPanel();

    ImGui::End();
    DrawLightGizmos();
    DrawTransformGizmo();
}

void Application::BuildScenePanel()
{
    if (!ImGui::CollapsingHeader("Scene", ImGuiTreeNodeFlags_DefaultOpen))
    {
        return;
    }

    if (ImGui::Button("Add Cube"))
    {
        scene_.SelectObject(scene_.AddMeshObject(MeshType::Cube));
    }
    ImGui::SameLine();
    if (ImGui::Button("Add Sphere"))
    {
        scene_.SelectObject(scene_.AddMeshObject(MeshType::Sphere));
    }
    if (ImGui::Button("Add Directional Light"))
    {
        scene_.SelectObject(scene_.AddLightObject(LightType::Directional));
    }
    ImGui::SameLine();
    if (ImGui::Button("Add Point Light"))
    {
        scene_.SelectObject(scene_.AddLightObject(LightType::Point));
    }

    const bool hasSelection = scene_.SelectedObjectId().has_value();
    if (!hasSelection)
    {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Delete Selected") && hasSelection)
    {
        scene_.DeleteObject(*scene_.SelectedObjectId());
    }
    if (!hasSelection)
    {
        ImGui::EndDisabled();
    }

    ImGui::Separator();
    for (const SceneObject& object : scene_.Objects())
    {
        const bool selected = scene_.SelectedObjectId() == object.id;
        std::string prefix = object.HasLight() ? "[Light] " : "[Mesh] ";
        if (object.HasRigidBody())
        {
            prefix += object.rigidBody->bodyType == BodyType::Dynamic ? "[Dynamic] " : "[Static] ";
        }
        else if (object.HasCollider())
        {
            prefix += "[Collider] ";
        }
        const std::string label = prefix + object.name + "##" + std::to_string(object.id);
        if (ImGui::Selectable(label.c_str(), selected))
        {
            scene_.SelectObject(object.id);
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("%s", SceneObjectKindName(object));
        }
    }
}

void Application::BuildInspectorPanel()
{
    if (!ImGui::CollapsingHeader("Inspector", ImGuiTreeNodeFlags_DefaultOpen))
    {
        return;
    }

    const std::optional<uint32_t> selectedId = scene_.SelectedObjectId();
    if (!selectedId.has_value())
    {
        ImGui::TextUnformatted("No object selected.");
        return;
    }

    SceneObject* object = scene_.FindObject(*selectedId);
    if (object == nullptr)
    {
        ImGui::TextUnformatted("No object selected.");
        return;
    }

    ImGui::Text("Name: %s", object->name.c_str());
    ImGui::Text("Type: %s", SceneObjectKindName(*object));

    ImGui::SeparatorText("Transform");
    ImGui::DragFloat3("Position", &object->transform.position.x, 0.05f);

    DirectX::XMFLOAT3 rotationDegrees{
        DirectX::XMConvertToDegrees(object->transform.rotationEuler.x),
        DirectX::XMConvertToDegrees(object->transform.rotationEuler.y),
        DirectX::XMConvertToDegrees(object->transform.rotationEuler.z)};
    if (ImGui::DragFloat3("Rotation", &rotationDegrees.x, 0.5f))
    {
        object->transform.rotationEuler = {
            DirectX::XMConvertToRadians(rotationDegrees.x),
            DirectX::XMConvertToRadians(rotationDegrees.y),
            DirectX::XMConvertToRadians(rotationDegrees.z)};
    }

    if (ImGui::DragFloat3("Scale", &object->transform.scale.x, 0.02f, 0.01f, 100.0f))
    {
        object->transform.scale.x = std::max(object->transform.scale.x, 0.01f);
        object->transform.scale.y = std::max(object->transform.scale.y, 0.01f);
        object->transform.scale.z = std::max(object->transform.scale.z, 0.01f);
    }

    if (object->mesh.has_value())
    {
        ImGui::SeparatorText("Mesh");
        ImGui::Text("Mesh: %s", MeshTypeName(object->mesh->fallbackMeshType));
        ImGui::Text("Mesh Asset Id: %u", object->mesh->meshAssetId);
        if (const MeshAssetInfo* asset = renderer_.FindMeshAsset(object->mesh->meshAssetId))
        {
            ImGui::Text("Asset: %s", asset->name.c_str());
            if (!asset->sourcePath.empty())
            {
                ImGui::TextWrapped("Source: %s", Narrow(asset->sourcePath).c_str());
            }
        }
        if (!object->mesh->sourcePath.empty())
        {
            ImGui::TextWrapped("Object Source: %s", Narrow(object->mesh->sourcePath).c_str());
        }
        if (object->mesh->missingMesh)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.42f, 0.25f, 1.0f), "Missing mesh: using cube placeholder");
        }

        ImGui::SeparatorText("Material");
        ImGui::Text("Material Id: %u", object->mesh->materialId);
        ImGui::TextUnformatted("Texture: uses current global texture");

        ImGui::SeparatorText("Physics");
        if (ImGui::Button("Make Player"))
        {
            scene_.SetPlayerObject(object->id);
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset Player Velocity"))
        {
            scene_.ResetPlayerPhysics();
        }

        bool hasRigidBody = object->rigidBody.has_value();
        if (ImGui::Checkbox("RigidBody", &hasRigidBody))
        {
            if (hasRigidBody)
            {
                object->rigidBody = RigidBodyComponent{};
            }
            else
            {
                object->rigidBody.reset();
            }
        }
        if (object->rigidBody.has_value())
        {
            int bodyType = static_cast<int>(object->rigidBody->bodyType);
            const char* bodyTypes[] = {"Static", "Dynamic"};
            if (ImGui::Combo("Body Type", &bodyType, bodyTypes, static_cast<int>(std::size(bodyTypes))))
            {
                object->rigidBody->bodyType = static_cast<BodyType>(bodyType);
            }
            ImGui::Text("Runtime: %s", BodyTypeName(object->rigidBody->bodyType));
            ImGui::Checkbox("Use Gravity", &object->rigidBody->useGravity);
            ImGui::DragFloat3("Velocity", &object->rigidBody->velocity.x, 0.05f);
            ImGui::DragFloat("Mass", &object->rigidBody->mass, 0.05f, 0.01f, 1000.0f);
            ImGui::DragFloat("Acceleration", &object->rigidBody->acceleration, 0.1f, 0.0f, 100.0f);
            ImGui::DragFloat("Max Speed", &object->rigidBody->maxSpeed, 0.1f, 0.1f, 200.0f);
            ImGui::DragFloat("Friction", &object->rigidBody->friction, 0.1f, 0.0f, 100.0f);
            ImGui::DragFloat("Turn Speed", &object->rigidBody->turnSpeed, 0.05f, 0.0f, 20.0f);
        }

        bool hasCollider = object->collider.has_value();
        if (ImGui::Checkbox("Collider", &hasCollider))
        {
            if (hasCollider)
            {
                ColliderComponent collider{};
                collider.type = object->mesh->fallbackMeshType == MeshType::Sphere ? ColliderType::Sphere : ColliderType::Box;
                object->collider = collider;
            }
            else
            {
                object->collider.reset();
            }
        }
        if (object->collider.has_value())
        {
            int colliderType = static_cast<int>(object->collider->type);
            const char* colliderTypes[] = {"Box", "Sphere"};
            if (ImGui::Combo("Collider Type", &colliderType, colliderTypes, static_cast<int>(std::size(colliderTypes))))
            {
                object->collider->type = static_cast<ColliderType>(colliderType);
            }
            ImGui::Text("Shape: %s", ColliderTypeName(object->collider->type));
            ImGui::Checkbox("Trigger", &object->collider->isTrigger);
            if (object->collider->type == ColliderType::Box)
            {
                if (ImGui::DragFloat3("Half Extents", &object->collider->halfExtents.x, 0.02f, 0.01f, 100.0f))
                {
                    object->collider->halfExtents.x = std::max(object->collider->halfExtents.x, 0.01f);
                    object->collider->halfExtents.y = std::max(object->collider->halfExtents.y, 0.01f);
                    object->collider->halfExtents.z = std::max(object->collider->halfExtents.z, 0.01f);
                }
            }
            else
            {
                ImGui::DragFloat("Radius", &object->collider->radius, 0.02f, 0.01f, 100.0f);
            }
        }
    }

    if (object->light.has_value())
    {
        ImGui::SeparatorText("Light");
        int lightType = static_cast<int>(object->light->type);
        const char* lightTypes[] = {"Directional", "Point"};
        if (ImGui::Combo("Light Type", &lightType, lightTypes, static_cast<int>(std::size(lightTypes))))
        {
            object->light->type = static_cast<LightType>(lightType);
        }
        ImGui::Checkbox("Enabled", &object->light->enabled);
        ImGui::ColorEdit3("Color", &object->light->color.x);
        ImGui::DragFloat("Intensity", &object->light->intensity, 0.05f, 0.0f, 20.0f);
        if (object->light->type == LightType::Point)
        {
            ImGui::DragFloat("Range", &object->light->range, 0.1f, 0.1f, 100.0f);
        }
        else
        {
            const DirectX::XMFLOAT3 direction = LightDirectionFromTransform(object->transform);
            ImGui::Text("Direction: %.2f, %.2f, %.2f", direction.x, direction.y, direction.z);
            ImGui::TextUnformatted("Rotate this object to change light direction.");
        }

        ImGui::SeparatorText("Shadow");
        ImGui::Checkbox("Enable Shadow", &object->light->shadow.enabled);
        if (object->light->shadow.enabled)
        {
            ImGui::DragFloat("Shadow Strength", &object->light->shadow.strength, 0.02f, 0.0f, 1.0f);
            ImGui::DragFloat("Shadow Bias", &object->light->shadow.bias, 0.0001f, 0.0f, 0.05f, "%.5f");
        }
    }

    ImGui::SeparatorText("Gizmo");
    int operation = static_cast<int>(gizmoOperation_);
    if (ImGui::RadioButton("Translate", operation == static_cast<int>(GizmoOperation::Translate)))
    {
        gizmoOperation_ = GizmoOperation::Translate;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Rotate", operation == static_cast<int>(GizmoOperation::Rotate)))
    {
        gizmoOperation_ = GizmoOperation::Rotate;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Scale", operation == static_cast<int>(GizmoOperation::Scale)))
    {
        gizmoOperation_ = GizmoOperation::Scale;
    }

    int mode = static_cast<int>(gizmoMode_);
    if (ImGui::RadioButton("Local", mode == static_cast<int>(GizmoMode::Local)))
    {
        gizmoMode_ = GizmoMode::Local;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("World", mode == static_cast<int>(GizmoMode::World)))
    {
        gizmoMode_ = GizmoMode::World;
    }
}

void Application::UpdateEditorPicking()
{
    if (input_.IsMouseCaptured() || !input_.WasMouseButtonPressed(0))
    {
        return;
    }

    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantCaptureMouse || ImGuizmo::IsOver() || ImGuizmo::IsUsing())
    {
        return;
    }

    const MousePosition mousePosition = input_.GetMousePosition();
    const float width = static_cast<float>(std::max(window_.Width(), 1u));
    const float height = static_cast<float>(std::max(window_.Height(), 1u));
    const float aspectRatio = width / height;
    scene_.SelectObject(scene_.PickObject(camera_.View(), camera_.Projection(aspectRatio), mousePosition.x, mousePosition.y, width, height));
}

void Application::DrawTransformGizmo()
{
    const std::optional<uint32_t> selectedId = scene_.SelectedObjectId();
    if (!selectedId.has_value())
    {
        return;
    }

    SceneObject* object = scene_.FindObject(*selectedId);
    if (object == nullptr)
    {
        return;
    }

    const float width = static_cast<float>(std::max(window_.Width(), 1u));
    const float height = static_cast<float>(std::max(window_.Height(), 1u));
    const float aspectRatio = width / height;

    DirectX::XMFLOAT4X4 view{};
    DirectX::XMFLOAT4X4 projection{};
    DirectX::XMFLOAT4X4 world{};
    DirectX::XMStoreFloat4x4(&view, camera_.View());
    DirectX::XMStoreFloat4x4(&projection, camera_.Projection(aspectRatio));
    DirectX::XMStoreFloat4x4(&world, object->transform.WorldMatrix());

    ImGuizmo::SetDrawlist(ImGui::GetForegroundDrawList());
    ImGuizmo::SetRect(0.0f, 0.0f, width, height);
    ImGuizmo::SetOrthographic(false);

    if (ImGuizmo::Manipulate(
            &view.m[0][0],
            &projection.m[0][0],
            ToImGuizmoOperation(gizmoOperation_),
            ToImGuizmoMode(gizmoMode_),
            &world.m[0][0]))
    {
        object->transform.SetFromMatrix(DirectX::XMLoadFloat4x4(&world));
    }
}

void Application::DrawLightGizmos()
{
    const float width = static_cast<float>(std::max(window_.Width(), 1u));
    const float height = static_cast<float>(std::max(window_.Height(), 1u));
    const float aspectRatio = width / height;
    const DirectX::XMMATRIX viewProjection = camera_.View() * camera_.Projection(aspectRatio);
    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    const std::optional<uint32_t> selectedId = scene_.SelectedObjectId();

    for (const SceneObject& object : scene_.Objects())
    {
        if (!object.light.has_value())
        {
            continue;
        }

        ImVec2 screenPosition{};
        if (!ProjectWorldPoint(object.transform.position, viewProjection, width, height, screenPosition))
        {
            continue;
        }

        const ImU32 lightColor = ImGui::ColorConvertFloat4ToU32(ImVec4(
            object.light->color.x,
            object.light->color.y,
            object.light->color.z,
            object.light->enabled ? 1.0f : 0.35f));
        const bool selected = selectedId == object.id;
        const float radius = selected ? 8.0f : 6.0f;

        drawList->AddCircleFilled(screenPosition, radius, lightColor, 18);
        drawList->AddCircle(screenPosition, radius + 2.0f, selected ? IM_COL32(255, 210, 80, 255) : IM_COL32(20, 20, 20, 220), 18, 2.0f);

        if (object.light->type == LightType::Directional)
        {
            const DirectX::XMFLOAT3 direction = LightDirectionFromTransform(object.transform);
            const DirectX::XMFLOAT3 endPoint{
                object.transform.position.x + direction.x * 1.4f,
                object.transform.position.y + direction.y * 1.4f,
                object.transform.position.z + direction.z * 1.4f};

            ImVec2 endScreen{};
            if (ProjectWorldPoint(endPoint, viewProjection, width, height, endScreen))
            {
                drawList->AddLine(screenPosition, endScreen, lightColor, 2.0f);
                drawList->AddCircleFilled(endScreen, 3.0f, lightColor, 10);
            }
        }
        else
        {
            drawList->AddText(ImVec2(screenPosition.x + 10.0f, screenPosition.y - 7.0f), lightColor, "Point");
        }
    }
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
        playCameraYaw_ = scene_.PlayerYaw();
        playCameraPitch_ = 0.22f;
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
    const std::wstring path = OpenFileDialog(
        window_.Handle(),
        L"Model Files\0*.fbx;*.obj\0FBX Files\0*.fbx\0OBJ Files\0*.obj\0All Files\0*.*\0\0");
    if (path.empty())
    {
        return;
    }

    const uint32_t meshAssetId = renderer_.LoadMeshAssetFromFile(path);
    if (meshAssetId != kInvalidMeshAssetId)
    {
        Transform transform{};
        const uint32_t objectId = scene_.AddMeshObject(meshAssetId, transform, FileStemUtf8(path), path, false);
        scene_.SelectObject(objectId);
        loadedModelLabel_ = L"Model: " + path;
        Log(LogLevel::Info, "Loaded external model as a scene object.");
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

void Application::ImportUnrealLevelJson()
{
    const std::wstring path = OpenFileDialog(
        window_.Handle(),
        L"Unreal Level JSON\0*.json\0All Files\0*.*\0\0");
    if (path.empty())
    {
        return;
    }

    std::ifstream file{std::filesystem::path(path)};
    if (!file.is_open())
    {
        Log(LogLevel::Error, "Failed to open Unreal level JSON.");
        return;
    }

    const std::string jsonText((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    const std::regex actorPattern(R"(\{[^{}]*(?:"fbxPath"|"staticMeshPath")[^{}]*\})");
    const std::filesystem::path jsonDirectory = std::filesystem::path(path).parent_path();
    uint32_t importedCount = 0;
    uint32_t missingCount = 0;

    for (std::sregex_iterator it(jsonText.begin(), jsonText.end(), actorPattern), end; it != end; ++it)
    {
        const std::string actorText = it->str();

        std::string name;
        if (!ExtractJsonString(actorText, "name", name) || name.empty())
        {
            name = "Unreal Static Mesh";
        }

        std::string fbxPathUtf8;
        ExtractJsonString(actorText, "fbxPath", fbxPathUtf8);
        std::wstring fbxPath = WidenUtf8(fbxPathUtf8);
        if (!fbxPath.empty())
        {
            std::filesystem::path resolvedPath(fbxPath);
            if (resolvedPath.is_relative())
            {
                resolvedPath = jsonDirectory / resolvedPath;
            }
            fbxPath = resolvedPath.lexically_normal().wstring();
        }

        DirectX::XMFLOAT3 locationCm{0.0f, 0.0f, 0.0f};
        DirectX::XMFLOAT3 rotationDegrees{0.0f, 0.0f, 0.0f};
        DirectX::XMFLOAT3 scale{1.0f, 1.0f, 1.0f};
        ExtractJsonVec3(actorText, "location", locationCm);
        ExtractJsonVec3(actorText, "rotationEuler", rotationDegrees);
        ExtractJsonVec3(actorText, "scale", scale);

        Transform transform{};
        transform.position = UnrealLocationToEngine(locationCm);
        transform.rotationEuler = UnrealRotationToEngineRadians(rotationDegrees);
        transform.scale = UnrealScaleToEngine(scale);

        uint32_t meshAssetId = kCubeMeshAssetId;
        bool missingMesh = true;
        if (!fbxPath.empty() && std::filesystem::exists(fbxPath))
        {
            const uint32_t loadedMeshId = renderer_.LoadMeshAssetFromFile(fbxPath);
            if (loadedMeshId != kInvalidMeshAssetId)
            {
                meshAssetId = loadedMeshId;
                missingMesh = false;
            }
        }

        scene_.AddMeshObject(meshAssetId, transform, name, fbxPath, missingMesh);
        ++importedCount;
        if (missingMesh)
        {
            ++missingCount;
        }
    }

    if (importedCount == 0)
    {
        Log(LogLevel::Warning, "Unreal level JSON did not contain any StaticMeshActor entries.");
        return;
    }

    loadedModelLabel_ = L"Unreal Level: " + path;
    std::ostringstream message;
    message << "Imported Unreal static mesh actors: " << importedCount;
    if (missingCount > 0)
    {
        message << " (" << missingCount << " missing meshes use cube placeholders)";
    }
    Log(LogLevel::Info, message.str());
}
} // namespace engine
