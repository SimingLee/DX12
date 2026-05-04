#include "engine/Scene.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>
#include <utility>

using namespace DirectX;

namespace engine
{
namespace
{
constexpr uint32_t kMaxSceneObjects = 128;
constexpr float kGravity = -9.81f;
constexpr float kGroundHeight = 0.0f;

float ClampScale(float value)
{
    return std::max(value, 0.01f);
}

MeshType MeshTypeForDefaults(uint32_t index)
{
    return index == 0 ? MeshType::Cube : MeshType::Sphere;
}

const char* MeshTypePrefix(MeshType type)
{
    switch (type)
    {
    case MeshType::Cube:
    case MeshType::ExternalModel:
        return "Cube";
    case MeshType::Sphere:
        return "Sphere";
    default:
        return "Object";
    }
}

uint32_t MeshAssetIdForType(MeshType type)
{
    return type == MeshType::Sphere ? kSphereMeshAssetId : kCubeMeshAssetId;
}

MeshType MeshTypeForAssetId(uint32_t meshAssetId, MeshType fallback)
{
    if (meshAssetId == kSphereMeshAssetId)
    {
        return MeshType::Sphere;
    }
    if (meshAssetId == kCubeMeshAssetId)
    {
        return MeshType::Cube;
    }
    return fallback;
}

const char* LightTypePrefix(LightType type)
{
    switch (type)
    {
    case LightType::Directional:
        return "Directional Light";
    case LightType::Point:
        return "Point Light";
    default:
        return "Light";
    }
}

bool IntersectAabb(FXMVECTOR origin, FXMVECTOR direction, float extent, float& distance)
{
    XMFLOAT3 o{};
    XMFLOAT3 d{};
    XMStoreFloat3(&o, origin);
    XMStoreFloat3(&d, direction);

    float tMin = 0.0f;
    float tMax = std::numeric_limits<float>::max();
    const float minBounds[3] = {-extent, -extent, -extent};
    const float maxBounds[3] = {extent, extent, extent};
    const float originValues[3] = {o.x, o.y, o.z};
    const float directionValues[3] = {d.x, d.y, d.z};

    for (int axis = 0; axis < 3; ++axis)
    {
        if (std::abs(directionValues[axis]) < 1.0e-6f)
        {
            if (originValues[axis] < minBounds[axis] || originValues[axis] > maxBounds[axis])
            {
                return false;
            }
            continue;
        }

        float t1 = (minBounds[axis] - originValues[axis]) / directionValues[axis];
        float t2 = (maxBounds[axis] - originValues[axis]) / directionValues[axis];
        if (t1 > t2)
        {
            std::swap(t1, t2);
        }

        tMin = std::max(tMin, t1);
        tMax = std::min(tMax, t2);
        if (tMin > tMax)
        {
            return false;
        }
    }

    distance = tMin;
    return true;
}

bool IntersectSphere(FXMVECTOR origin, FXMVECTOR direction, float radius, float& distance)
{
    const float b = XMVectorGetX(XMVector3Dot(origin, direction));
    const float c = XMVectorGetX(XMVector3Dot(origin, origin)) - radius * radius;
    const float discriminant = b * b - c;
    if (discriminant < 0.0f)
    {
        return false;
    }

    const float sqrtDiscriminant = std::sqrt(discriminant);
    const float nearDistance = -b - sqrtDiscriminant;
    const float farDistance = -b + sqrtDiscriminant;
    distance = nearDistance >= 0.0f ? nearDistance : farDistance;
    return distance >= 0.0f;
}

float AbsScale(float value)
{
    return std::max(std::abs(value), 0.01f);
}

float MoveTowardZero(float value, float amount)
{
    if (value > amount)
    {
        return value - amount;
    }
    if (value < -amount)
    {
        return value + amount;
    }
    return 0.0f;
}

struct ColliderBounds
{
    ColliderType type = ColliderType::Box;
    XMFLOAT3 center{};
    XMFLOAT3 halfExtents{1.0f, 1.0f, 1.0f};
    float radius = 1.0f;
};

ColliderBounds BuildColliderBounds(const SceneObject& object, const XMFLOAT3& position)
{
    ColliderBounds bounds{};
    if (!object.collider.has_value())
    {
        return bounds;
    }

    const ColliderComponent& collider = *object.collider;
    bounds.type = collider.type;
    bounds.center = position;
    bounds.halfExtents = {
        collider.halfExtents.x * AbsScale(object.transform.scale.x),
        collider.halfExtents.y * AbsScale(object.transform.scale.y),
        collider.halfExtents.z * AbsScale(object.transform.scale.z)};
    bounds.radius = collider.radius * std::max({
        AbsScale(object.transform.scale.x),
        AbsScale(object.transform.scale.y),
        AbsScale(object.transform.scale.z)});
    return bounds;
}

ColliderBounds BuildColliderBounds(const SceneObject& object)
{
    return BuildColliderBounds(object, object.transform.position);
}

bool AabbIntersectsAabb(const ColliderBounds& a, const ColliderBounds& b)
{
    return std::abs(a.center.x - b.center.x) <= a.halfExtents.x + b.halfExtents.x &&
        std::abs(a.center.y - b.center.y) <= a.halfExtents.y + b.halfExtents.y &&
        std::abs(a.center.z - b.center.z) <= a.halfExtents.z + b.halfExtents.z;
}

float DistanceSquared(const XMFLOAT3& a, const XMFLOAT3& b)
{
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    const float dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

bool SphereIntersectsSphere(const ColliderBounds& a, const ColliderBounds& b)
{
    const float radius = a.radius + b.radius;
    return DistanceSquared(a.center, b.center) <= radius * radius;
}

bool SphereIntersectsAabb(const ColliderBounds& sphere, const ColliderBounds& box)
{
    const float closestX = std::clamp(sphere.center.x, box.center.x - box.halfExtents.x, box.center.x + box.halfExtents.x);
    const float closestY = std::clamp(sphere.center.y, box.center.y - box.halfExtents.y, box.center.y + box.halfExtents.y);
    const float closestZ = std::clamp(sphere.center.z, box.center.z - box.halfExtents.z, box.center.z + box.halfExtents.z);
    const XMFLOAT3 closestPoint{closestX, closestY, closestZ};
    return DistanceSquared(sphere.center, closestPoint) <= sphere.radius * sphere.radius;
}

bool CollidersIntersect(const ColliderBounds& a, const ColliderBounds& b)
{
    if (a.type == ColliderType::Sphere && b.type == ColliderType::Sphere)
    {
        return SphereIntersectsSphere(a, b);
    }
    if (a.type == ColliderType::Sphere && b.type == ColliderType::Box)
    {
        return SphereIntersectsAabb(a, b);
    }
    if (a.type == ColliderType::Box && b.type == ColliderType::Sphere)
    {
        return SphereIntersectsAabb(b, a);
    }
    return AabbIntersectsAabb(a, b);
}
} // namespace

XMMATRIX Transform::WorldMatrix() const
{
    return XMMatrixScaling(ClampScale(scale.x), ClampScale(scale.y), ClampScale(scale.z)) *
        XMMatrixRotationRollPitchYaw(rotationEuler.x, rotationEuler.y, rotationEuler.z) *
        XMMatrixTranslation(position.x, position.y, position.z);
}

void Transform::SetFromMatrix(const XMMATRIX& matrix)
{
    XMVECTOR scaleVector{};
    XMVECTOR rotationQuaternion{};
    XMVECTOR translationVector{};
    if (!XMMatrixDecompose(&scaleVector, &rotationQuaternion, &translationVector, matrix))
    {
        return;
    }

    XMStoreFloat3(&position, translationVector);
    XMStoreFloat3(&scale, scaleVector);
    scale.x = ClampScale(scale.x);
    scale.y = ClampScale(scale.y);
    scale.z = ClampScale(scale.z);

    const XMMATRIX rotationMatrix = XMMatrixRotationQuaternion(rotationQuaternion);
    rotationEuler.x = std::atan2f(
        -XMVectorGetZ(rotationMatrix.r[1]),
        XMVectorGetZ(rotationMatrix.r[2]));
    rotationEuler.y = std::asinf(XMVectorGetZ(rotationMatrix.r[0]));
    rotationEuler.z = std::atan2f(
        -XMVectorGetY(rotationMatrix.r[0]),
        XMVectorGetX(rotationMatrix.r[0]));
}

Scene::Scene()
{
    AddMeshObject(MeshTypeForDefaults(0));
    AddMeshObject(MeshTypeForDefaults(1));
    AddLightObject(LightType::Directional);
    if (SceneObject* firstMesh = FirstMeshObject())
    {
        playerObjectId_ = firstMesh->id;
        selectedObjectId_ = playerObjectId_;
        playerPosition_ = firstMesh->transform.position;
        playerYaw_ = firstMesh->transform.rotationEuler.y;
    }
}

void Scene::Update(float deltaTime)
{
    (void)deltaTime;
}

void Scene::UpdatePlayer(const InputSystem& input, float deltaTime)
{
    UpdatePlayPhysics(input, deltaTime);
}

void Scene::UpdatePlayPhysics(const InputSystem& input, float deltaTime)
{
    SceneObject* player = PlayerObject();
    if (player == nullptr)
    {
        return;
    }
    EnsureDefaultPhysics(*player, true);

    RigidBodyComponent& rigidBody = *player->rigidBody;
    if (rigidBody.bodyType != BodyType::Dynamic)
    {
        SetPlayerObject(player->id);
    }

    const float dt = std::clamp(deltaTime, 0.0f, 0.05f);
    if (dt <= 0.0f)
    {
        return;
    }

    const float turnInput =
        (input.IsKeyDown('D') ? 1.0f : 0.0f) -
        (input.IsKeyDown('A') ? 1.0f : 0.0f);
    const float throttleInput =
        (input.IsKeyDown('W') ? 1.0f : 0.0f) -
        (input.IsKeyDown('S') ? 1.0f : 0.0f);

    const float horizontalSpeed = std::sqrt(
        rigidBody.velocity.x * rigidBody.velocity.x +
        rigidBody.velocity.z * rigidBody.velocity.z);
    const float steeringScale = horizontalSpeed > 0.05f ?
        std::clamp(horizontalSpeed / std::max(rigidBody.maxSpeed, 0.01f), 0.25f, 1.0f) :
        0.25f;
    playerYaw_ += turnInput * rigidBody.turnSpeed * steeringScale * dt;
    player->transform.rotationEuler.y = playerYaw_;

    const XMVECTOR forward = XMVector3Normalize(XMVectorSet(std::sinf(playerYaw_), 0.0f, std::cosf(playerYaw_), 0.0f));
    if (std::abs(throttleInput) > 0.0f)
    {
        XMFLOAT3 forwardValues{};
        XMStoreFloat3(&forwardValues, forward);
        rigidBody.velocity.x += forwardValues.x * rigidBody.acceleration * throttleInput * dt;
        rigidBody.velocity.z += forwardValues.z * rigidBody.acceleration * throttleInput * dt;
    }
    else
    {
        const float frictionStep = std::max(rigidBody.friction, 0.0f) * dt;
        rigidBody.velocity.x = MoveTowardZero(rigidBody.velocity.x, frictionStep);
        rigidBody.velocity.z = MoveTowardZero(rigidBody.velocity.z, frictionStep);
    }

    const float currentSpeed = std::sqrt(
        rigidBody.velocity.x * rigidBody.velocity.x +
        rigidBody.velocity.z * rigidBody.velocity.z);
    if (currentSpeed > rigidBody.maxSpeed && currentSpeed > 0.0f)
    {
        const float scale = rigidBody.maxSpeed / currentSpeed;
        rigidBody.velocity.x *= scale;
        rigidBody.velocity.z *= scale;
    }

    if (rigidBody.useGravity)
    {
        rigidBody.velocity.y += kGravity * dt;
    }
    else
    {
        rigidBody.velocity.y = 0.0f;
    }

    XMFLOAT3 position = player->transform.position;
    XMFLOAT3 candidate = position;
    candidate.x += rigidBody.velocity.x * dt;
    if (!WouldCollide(*player, candidate))
    {
        position.x = candidate.x;
    }
    else
    {
        rigidBody.velocity.x = 0.0f;
    }

    candidate = position;
    candidate.z += rigidBody.velocity.z * dt;
    if (!WouldCollide(*player, candidate))
    {
        position.z = candidate.z;
    }
    else
    {
        rigidBody.velocity.z = 0.0f;
    }

    position.y += rigidBody.velocity.y * dt;
    if (position.y < kGroundHeight)
    {
        position.y = kGroundHeight;
        if (rigidBody.velocity.y < 0.0f)
        {
            rigidBody.velocity.y = 0.0f;
        }
    }

    player->transform.position = position;
    playerPosition_ = position;
}

void Scene::EnsureDefaultPhysics(SceneObject& object, bool makeDynamicPlayer)
{
    if (!object.HasMesh())
    {
        return;
    }

    const MeshType meshType = MeshTypeForAssetId(object.mesh->meshAssetId, object.mesh->fallbackMeshType);
    if (!object.collider.has_value())
    {
        ColliderComponent collider{};
        collider.type = meshType == MeshType::Sphere ? ColliderType::Sphere : ColliderType::Box;
        collider.halfExtents = {1.0f, 1.0f, 1.0f};
        collider.radius = 1.0f;
        object.collider = collider;
    }

    if (!object.rigidBody.has_value())
    {
        RigidBodyComponent rigidBody{};
        rigidBody.bodyType = makeDynamicPlayer ? BodyType::Dynamic : BodyType::Static;
        rigidBody.useGravity = false;
        object.rigidBody = rigidBody;
    }
    else if (makeDynamicPlayer)
    {
        object.rigidBody->bodyType = BodyType::Dynamic;
    }
}

void Scene::SetPlayerObject(uint32_t id)
{
    SceneObject* object = FindObject(id);
    if (object == nullptr || !object->HasMesh())
    {
        return;
    }

    if (SceneObject* currentPlayer = PlayerObject(); currentPlayer != nullptr && currentPlayer->id != id)
    {
        EnsureDefaultPhysics(*currentPlayer, false);
        currentPlayer->rigidBody->bodyType = BodyType::Static;
        currentPlayer->rigidBody->velocity = {0.0f, 0.0f, 0.0f};
    }

    playerObjectId_ = id;
    EnsureDefaultPhysics(*object, true);
    playerYaw_ = object->transform.rotationEuler.y;
    playerPosition_ = object->transform.position;
}

void Scene::ResetPlayerPhysics()
{
    if (SceneObject* player = PlayerObject(); player != nullptr)
    {
        EnsureDefaultPhysics(*player, true);
        player->rigidBody->velocity = {0.0f, 0.0f, 0.0f};
        playerYaw_ = player->transform.rotationEuler.y;
        playerPosition_ = player->transform.position;
    }
}

uint32_t Scene::AddObject(MeshType type)
{
    return AddMeshObject(type);
}

uint32_t Scene::AddMeshObject(MeshType type)
{
    if (objects_.size() >= kMaxSceneObjects)
    {
        return 0;
    }

    SceneObject object{};
    object.id = nextObjectId_++;
    object.mesh = MeshComponent{MeshAssetIdForType(type), type, 0};
    object.name = MakeObjectName(type);
    object.transform.position = {
        -3.0f + static_cast<float>(MeshObjectCount()) * 3.0f,
        0.0f,
        0.0f};

    const bool makeDynamicPlayer = playerObjectId_ == 0;
    EnsureDefaultPhysics(object, makeDynamicPlayer);

    objects_.push_back(object);
    if (makeDynamicPlayer)
    {
        playerObjectId_ = object.id;
        playerPosition_ = object.transform.position;
        playerYaw_ = object.transform.rotationEuler.y;
    }

    return object.id;
}

uint32_t Scene::AddMeshObject(
    uint32_t meshAssetId,
    const Transform& transform,
    std::string name,
    const std::wstring& sourcePath,
    bool missingMesh)
{
    if (objects_.size() >= kMaxSceneObjects)
    {
        return 0;
    }

    SceneObject object{};
    object.id = nextObjectId_++;
    object.mesh = MeshComponent{
        meshAssetId == kInvalidMeshAssetId ? kCubeMeshAssetId : meshAssetId,
        meshAssetId == kSphereMeshAssetId ? MeshType::Sphere : MeshType::ExternalModel,
        0,
        sourcePath,
        missingMesh};
    object.name = name.empty() ? MakeObjectName(MeshType::ExternalModel) : std::move(name);
    object.transform = transform;
    EnsureDefaultPhysics(object, false);

    objects_.push_back(object);
    if (playerObjectId_ == 0)
    {
        SetPlayerObject(object.id);
    }

    return object.id;
}

uint32_t Scene::AddLightObject(LightType type)
{
    if (objects_.size() >= kMaxSceneObjects)
    {
        return 0;
    }

    SceneObject object{};
    object.id = nextObjectId_++;
    object.light = LightComponent{};
    object.light->type = type;
    object.name = MakeLightName(type);
    object.transform.position = type == LightType::Directional ?
        XMFLOAT3{-2.5f, 3.0f, -2.5f} :
        XMFLOAT3{0.0f, 2.5f, 0.0f};
    object.transform.scale = {1.0f, 1.0f, 1.0f};
    object.transform.rotationEuler = type == LightType::Directional ?
        XMFLOAT3{0.8f, -0.72f, 0.0f} :
        XMFLOAT3{0.0f, 0.0f, 0.0f};
    if (type == LightType::Point)
    {
        object.light->color = {0.8f, 0.92f, 1.0f};
        object.light->intensity = 2.5f;
        object.light->range = 6.0f;
    }

    objects_.push_back(object);
    return object.id;
}

void Scene::DeleteObject(uint32_t id)
{
    const auto objectIt = std::find_if(objects_.begin(), objects_.end(), [id](const SceneObject& object) {
        return object.id == id;
    });
    if (objectIt == objects_.end())
    {
        return;
    }

    if (objectIt->HasMesh() && MeshObjectCount() <= 1)
    {
        return;
    }

    objects_.erase(objectIt);
    if (selectedObjectId_ == id)
    {
        selectedObjectId_.reset();
    }
    if (playerObjectId_ == id)
    {
        if (SceneObject* firstMesh = FirstMeshObject())
        {
            SetPlayerObject(firstMesh->id);
        }
        else
        {
            playerObjectId_ = 0;
        }
    }
}

SceneObject* Scene::FindObject(uint32_t id)
{
    const auto objectIt = std::find_if(objects_.begin(), objects_.end(), [id](const SceneObject& object) {
        return object.id == id;
    });
    return objectIt != objects_.end() ? &(*objectIt) : nullptr;
}

const SceneObject* Scene::FindObject(uint32_t id) const
{
    const auto objectIt = std::find_if(objects_.begin(), objects_.end(), [id](const SceneObject& object) {
        return object.id == id;
    });
    return objectIt != objects_.end() ? &(*objectIt) : nullptr;
}

void Scene::SelectObject(std::optional<uint32_t> id)
{
    if (!id.has_value() || FindObject(*id) == nullptr)
    {
        selectedObjectId_.reset();
        return;
    }

    selectedObjectId_ = id;
}

std::optional<uint32_t> Scene::PickObject(
    const XMMATRIX& view,
    const XMMATRIX& projection,
    float mouseX,
    float mouseY,
    float viewportWidth,
    float viewportHeight) const
{
    if (viewportWidth <= 0.0f || viewportHeight <= 0.0f)
    {
        return std::nullopt;
    }

    XMFLOAT4X4 projectionValues{};
    XMStoreFloat4x4(&projectionValues, projection);
    const float x = (2.0f * mouseX / viewportWidth - 1.0f) / projectionValues._11;
    const float y = (-2.0f * mouseY / viewportHeight + 1.0f) / projectionValues._22;
    const XMVECTOR rayOriginView = XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f);
    const XMVECTOR rayDirectionView = XMVector3Normalize(XMVectorSet(x, y, 1.0f, 0.0f));
    const XMMATRIX inverseView = XMMatrixInverse(nullptr, view);
    const XMVECTOR rayOriginWorld = XMVector3TransformCoord(rayOriginView, inverseView);
    const XMVECTOR rayDirectionWorld = XMVector3Normalize(XMVector3TransformNormal(rayDirectionView, inverseView));

    float closestDistance = std::numeric_limits<float>::max();
    std::optional<uint32_t> closestId{};
    for (const SceneObject& object : objects_)
    {
        if (!object.selectable)
        {
            continue;
        }

        float distance = 0.0f;
        if (IntersectObject(object, rayOriginWorld, rayDirectionWorld, distance) && distance < closestDistance)
        {
            closestDistance = distance;
            closestId = object.id;
        }
    }

    return closestId;
}

SceneObject* Scene::PlayerObject()
{
    SceneObject* object = FindObject(playerObjectId_);
    return object != nullptr && object->HasMesh() ? object : nullptr;
}

const SceneObject* Scene::PlayerObject() const
{
    const SceneObject* object = FindObject(playerObjectId_);
    return object != nullptr && object->HasMesh() ? object : nullptr;
}

std::string Scene::MakeObjectName(MeshType type) const
{
    const char* prefix = MeshTypePrefix(type);
    uint32_t matchingCount = 0;
    for (const SceneObject& object : objects_)
    {
        if (object.mesh.has_value() &&
            (object.mesh->fallbackMeshType == type ||
                (type == MeshType::ExternalModel && object.mesh->fallbackMeshType == MeshType::Cube)))
        {
            ++matchingCount;
        }
    }

    std::ostringstream stream;
    stream << prefix << " " << matchingCount + 1;
    return stream.str();
}

std::string Scene::MakeLightName(LightType type) const
{
    const char* prefix = LightTypePrefix(type);
    uint32_t matchingCount = 0;
    for (const SceneObject& object : objects_)
    {
        if (object.light.has_value() && object.light->type == type)
        {
            ++matchingCount;
        }
    }

    std::ostringstream stream;
    stream << prefix << " " << matchingCount + 1;
    return stream.str();
}

uint32_t Scene::MeshObjectCount() const
{
    return static_cast<uint32_t>(std::count_if(objects_.begin(), objects_.end(), [](const SceneObject& object) {
        return object.HasMesh();
    }));
}

SceneObject* Scene::FirstMeshObject()
{
    const auto objectIt = std::find_if(objects_.begin(), objects_.end(), [](const SceneObject& object) {
        return object.HasMesh();
    });
    return objectIt != objects_.end() ? &(*objectIt) : nullptr;
}

const SceneObject* Scene::FirstMeshObject() const
{
    const auto objectIt = std::find_if(objects_.begin(), objects_.end(), [](const SceneObject& object) {
        return object.HasMesh();
    });
    return objectIt != objects_.end() ? &(*objectIt) : nullptr;
}

bool Scene::WouldCollide(const SceneObject& movingObject, const XMFLOAT3& candidatePosition) const
{
    if (!movingObject.collider.has_value() || movingObject.collider->isTrigger)
    {
        return false;
    }

    const ColliderBounds movingBounds = BuildColliderBounds(movingObject, candidatePosition);
    for (const SceneObject& other : objects_)
    {
        if (other.id == movingObject.id || !other.collider.has_value() || other.collider->isTrigger)
        {
            continue;
        }
        if (other.rigidBody.has_value() && other.rigidBody->bodyType == BodyType::Dynamic)
        {
            continue;
        }

        if (CollidersIntersect(movingBounds, BuildColliderBounds(other)))
        {
            return true;
        }
    }

    return false;
}

bool Scene::IntersectObject(
    const SceneObject& object,
    FXMVECTOR rayOrigin,
    FXMVECTOR rayDirection,
    float& outDistance) const
{
    if (object.light.has_value())
    {
        const XMMATRIX inverseTranslation = XMMatrixTranslation(
            -object.transform.position.x,
            -object.transform.position.y,
            -object.transform.position.z);
        const XMVECTOR localOrigin = XMVector3TransformCoord(rayOrigin, inverseTranslation);
        const XMVECTOR localDirection = XMVector3Normalize(rayDirection);
        const float radius = object.light->type == LightType::Directional ? 0.5f : 0.4f;
        return IntersectSphere(localOrigin, localDirection, radius, outDistance);
    }

    if (!object.mesh.has_value())
    {
        return false;
    }

    const XMMATRIX inverseWorld = XMMatrixInverse(nullptr, object.transform.WorldMatrix());
    const XMVECTOR localOrigin = XMVector3TransformCoord(rayOrigin, inverseWorld);
    const XMVECTOR localDirection = XMVector3Normalize(XMVector3TransformNormal(rayDirection, inverseWorld));

    switch (MeshTypeForAssetId(object.mesh->meshAssetId, object.mesh->fallbackMeshType))
    {
    case MeshType::Sphere:
        return IntersectSphere(localOrigin, localDirection, 1.0f, outDistance);
    case MeshType::Cube:
    case MeshType::ExternalModel:
    default:
        return IntersectAabb(localOrigin, localDirection, 1.0f, outDistance);
    }
}
} // namespace engine
