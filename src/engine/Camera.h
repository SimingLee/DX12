#pragma once

#include "engine/InputSystem.h"

#include <DirectXMath.h>

namespace engine
{
class Camera
{
public:
    Camera();

    void Update(const InputSystem& input, float deltaTime);
    void SetTransform(const DirectX::XMFLOAT3& position, float yaw, float pitch);

    DirectX::XMMATRIX View() const;
    DirectX::XMMATRIX Projection(float aspectRatio) const;
    DirectX::XMFLOAT3 Position() const { return position_; }
    float Yaw() const { return yaw_; }
    float Pitch() const { return pitch_; }

private:
    DirectX::XMFLOAT3 position_{};
    float yaw_ = 0.0f;
    float pitch_ = 0.0f;
    float moveSpeed_ = 4.0f;
    float rotationSpeed_ = 0.0025f;
};
} // namespace engine
