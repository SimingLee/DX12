#pragma once

#include "engine/InputSystem.h"

#include <DirectXMath.h>

namespace engine
{
class Scene
{
public:
    void Update(float deltaTime);
    void UpdatePlayer(const InputSystem& input, float deltaTime);
    DirectX::XMMATRIX CubeWorldMatrix() const;
    DirectX::XMMATRIX SphereWorldMatrix() const;
    DirectX::XMFLOAT3 PlayerPosition() const { return playerPosition_; }
    float PlayerYaw() const { return playerYaw_; }

private:
    float rotation_ = 0.0f;
    DirectX::XMFLOAT3 playerPosition_{-1.75f, 0.0f, 0.0f};
    float playerYaw_ = 0.0f;
};
} // namespace engine
