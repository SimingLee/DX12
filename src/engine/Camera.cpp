#include "engine/Camera.h"

#include <algorithm>
#include <cmath>

using namespace DirectX;

namespace engine
{
Camera::Camera()
    : position_(0.0f, 1.2f, -6.0f)
{
}

void Camera::Update(const InputSystem& input, float deltaTime)
{
    const MouseDelta mouseDelta = input.GetMouseDelta();
    if (input.IsMouseCaptured())
    {
        yaw_ += mouseDelta.x * rotationSpeed_;
        pitch_ += mouseDelta.y * rotationSpeed_;
        pitch_ = std::clamp(pitch_, -XM_PIDIV2 + 0.01f, XM_PIDIV2 - 0.01f);
    }

    const XMVECTOR forward = XMVector3Normalize(XMVectorSet(
        std::sinf(yaw_) * std::cosf(pitch_),
        -std::sinf(pitch_),
        std::cosf(yaw_) * std::cosf(pitch_),
        0.0f));
    const XMVECTOR right = XMVector3Normalize(XMVector3Cross(XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f), forward));
    const XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

    XMVECTOR position = XMLoadFloat3(&position_);
    const float velocity = moveSpeed_ * deltaTime;

    if (input.IsKeyDown('W'))
    {
        position += forward * velocity;
    }
    if (input.IsKeyDown('S'))
    {
        position -= forward * velocity;
    }
    if (input.IsKeyDown('A'))
    {
        position -= right * velocity;
    }
    if (input.IsKeyDown('D'))
    {
        position += right * velocity;
    }
    if (input.IsKeyDown('Q'))
    {
        position -= up * velocity;
    }
    if (input.IsKeyDown('E'))
    {
        position += up * velocity;
    }

    XMStoreFloat3(&position_, position);
}

void Camera::SetTransform(const XMFLOAT3& position, float yaw, float pitch)
{
    position_ = position;
    yaw_ = yaw;
    pitch_ = pitch;
}

XMMATRIX Camera::View() const
{
    const XMVECTOR position = XMLoadFloat3(&position_);
    const XMVECTOR forward = XMVector3Normalize(XMVectorSet(
        std::sinf(yaw_) * std::cosf(pitch_),
        -std::sinf(pitch_),
        std::cosf(yaw_) * std::cosf(pitch_),
        0.0f));
    const XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    return XMMatrixLookToLH(position, forward, up);
}

XMMATRIX Camera::Projection(float aspectRatio) const
{
    return XMMatrixPerspectiveFovLH(XM_PIDIV4, aspectRatio, 0.1f, 100.0f);
}
} // namespace engine
