#pragma once

#include "engine/InputSystem.h"

#include <DirectXMath.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace engine
{
inline constexpr uint32_t kInvalidMeshAssetId = 0;
inline constexpr uint32_t kCubeMeshAssetId = 1;
inline constexpr uint32_t kSphereMeshAssetId = 2;

enum class MeshType
{
    Cube,
    Sphere,
    ExternalModel
};

enum class LightType
{
    Directional,
    Point
};

enum class BodyType
{
    Static,
    Dynamic
};

enum class ColliderType
{
    Box,
    Sphere
};

struct Transform
{
    DirectX::XMFLOAT3 position{0.0f, 0.0f, 0.0f};
    DirectX::XMFLOAT3 rotationEuler{0.0f, 0.0f, 0.0f};
    DirectX::XMFLOAT3 scale{1.0f, 1.0f, 1.0f};

    DirectX::XMMATRIX WorldMatrix() const;
    void SetFromMatrix(const DirectX::XMMATRIX& matrix);
};

struct MeshComponent
{
    uint32_t meshAssetId = kCubeMeshAssetId;
    MeshType fallbackMeshType = MeshType::Cube;
    uint32_t materialId = 0;
    std::wstring sourcePath{};
    bool missingMesh = false;
};

struct ColliderComponent
{
    ColliderType type = ColliderType::Box;
    DirectX::XMFLOAT3 halfExtents{1.0f, 1.0f, 1.0f};
    float radius = 1.0f;
    bool isTrigger = false;
};

struct RigidBodyComponent
{
    BodyType bodyType = BodyType::Static;
    DirectX::XMFLOAT3 velocity{0.0f, 0.0f, 0.0f};
    bool useGravity = false;
    float mass = 1.0f;
    float acceleration = 8.0f;
    float maxSpeed = 12.0f;
    float friction = 5.0f;
    float turnSpeed = 2.4f;
};

struct ShadowSettings
{
    bool enabled = true;
    float strength = 0.65f;
    float bias = 0.0035f;
};

struct LightComponent
{
    LightType type = LightType::Directional;
    DirectX::XMFLOAT3 color{1.0f, 0.96f, 0.88f};
    float intensity = 1.0f;
    float range = 8.0f;
    bool enabled = true;
    ShadowSettings shadow{};
};

struct SceneObject
{
    uint32_t id = 0;
    std::string name{};
    Transform transform{};
    std::optional<MeshComponent> mesh{};
    std::optional<ColliderComponent> collider{};
    std::optional<RigidBodyComponent> rigidBody{};
    std::optional<LightComponent> light{};
    bool selectable = true;

    bool HasMesh() const { return mesh.has_value(); }
    bool HasCollider() const { return collider.has_value(); }
    bool HasRigidBody() const { return rigidBody.has_value(); }
    bool HasLight() const { return light.has_value(); }
};

class Scene
{
public:
    Scene();

    void Update(float deltaTime);
    void UpdatePlayer(const InputSystem& input, float deltaTime);
    void UpdatePlayPhysics(const InputSystem& input, float deltaTime);
    void EnsureDefaultPhysics(SceneObject& object, bool makeDynamicPlayer);
    void SetPlayerObject(uint32_t id);
    void ResetPlayerPhysics();
    uint32_t AddObject(MeshType type);
    uint32_t AddMeshObject(MeshType type);
    uint32_t AddMeshObject(
        uint32_t meshAssetId,
        const Transform& transform,
        std::string name,
        const std::wstring& sourcePath = {},
        bool missingMesh = false);
    uint32_t AddLightObject(LightType type);
    void DeleteObject(uint32_t id);
    const std::vector<SceneObject>& Objects() const { return objects_; }
    SceneObject* FindObject(uint32_t id);
    const SceneObject* FindObject(uint32_t id) const;
    std::optional<uint32_t> SelectedObjectId() const { return selectedObjectId_; }
    void SelectObject(std::optional<uint32_t> id);
    std::optional<uint32_t> PickObject(
        const DirectX::XMMATRIX& view,
        const DirectX::XMMATRIX& projection,
        float mouseX,
        float mouseY,
        float viewportWidth,
        float viewportHeight) const;
    SceneObject* PlayerObject();
    const SceneObject* PlayerObject() const;
    DirectX::XMFLOAT3 PlayerPosition() const { return playerPosition_; }
    float PlayerYaw() const { return playerYaw_; }

private:
    std::string MakeObjectName(MeshType type) const;
    std::string MakeLightName(LightType type) const;
    uint32_t MeshObjectCount() const;
    SceneObject* FirstMeshObject();
    const SceneObject* FirstMeshObject() const;
    bool WouldCollide(const SceneObject& movingObject, const DirectX::XMFLOAT3& candidatePosition) const;
    bool IntersectObject(
        const SceneObject& object,
        DirectX::FXMVECTOR rayOrigin,
        DirectX::FXMVECTOR rayDirection,
        float& outDistance) const;

    std::vector<SceneObject> objects_{};
    uint32_t nextObjectId_ = 1;
    std::optional<uint32_t> selectedObjectId_{};
    uint32_t playerObjectId_ = 0;
    DirectX::XMFLOAT3 playerPosition_{0.0f, 0.0f, 0.0f};
    float playerYaw_ = 0.0f;
};
} // namespace engine
