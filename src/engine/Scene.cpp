#include "engine/Scene.h"

#include <cmath>

using namespace DirectX;

namespace engine
{
void Scene::Update(float deltaTime)
{
    (void)deltaTime;
}

void Scene::UpdatePlayer(const InputSystem& input, float deltaTime)
{
    constexpr float movementSpeed = 3.5f;
    constexpr float turnSpeed = 0.0025f;

    const MouseDelta mouseDelta = input.GetMouseDelta();
    playerYaw_ += mouseDelta.x * turnSpeed;

    const XMVECTOR forward = XMVector3Normalize(XMVectorSet(std::sinf(playerYaw_), 0.0f, std::cosf(playerYaw_), 0.0f));
    const XMVECTOR right = XMVector3Normalize(XMVectorSet(std::cosf(playerYaw_), 0.0f, -std::sinf(playerYaw_), 0.0f));
    XMVECTOR position = XMLoadFloat3(&playerPosition_);

    if (input.IsKeyDown('W'))
    {
        position += forward * (movementSpeed * deltaTime);
    }
    if (input.IsKeyDown('S'))
    {
        position -= forward * (movementSpeed * deltaTime);
    }
    if (input.IsKeyDown('D'))
    {
        position += right * (movementSpeed * deltaTime);
    }
    if (input.IsKeyDown('A'))
    {
        position -= right * (movementSpeed * deltaTime);
    }

    XMStoreFloat3(&playerPosition_, position);
}

XMMATRIX Scene::CubeWorldMatrix() const
{
    return XMMatrixRotationY(playerYaw_) *
        XMMatrixTranslation(playerPosition_.x, playerPosition_.y, playerPosition_.z);
}

XMMATRIX Scene::SphereWorldMatrix() const
{
    return XMMatrixTranslation(1.75f, 0.0f, 0.0f);
}
} // namespace engine
