#pragma once

#include <Windows.h>

#include <array>

namespace engine
{
struct MouseDelta
{
    float x = 0.0f;
    float y = 0.0f;
};

struct MousePosition
{
    float x = 0.0f;
    float y = 0.0f;
};

class InputSystem
{
public:
    void BeginFrame();
    void HandleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

    bool IsKeyDown(uint32_t key) const;
    bool WasKeyPressed(uint32_t key) const;
    bool IsMouseButtonDown(uint32_t button) const;
    bool WasMouseButtonPressed(uint32_t button) const;
    MouseDelta GetMouseDelta() const;
    MousePosition GetMousePosition() const;

    bool IsMouseCaptured() const { return mouseCaptured_; }
    void CaptureMouse(HWND hwnd, bool forced = true);
    void ReleaseMouseCapture(HWND hwnd);

private:
    void CenterCursor(HWND hwnd);
    void ShowCapturedCursor(bool visible);
    void ClearState();

    std::array<bool, 256> currentKeys_{};
    std::array<bool, 256> previousKeys_{};
    std::array<bool, 3> currentMouseButtons_{};
    std::array<bool, 3> previousMouseButtons_{};
    MouseDelta mouseDelta_{};
    MousePosition mousePosition_{};
    bool mouseCaptured_ = false;
    bool forcedCapture_ = false;
    bool ignoreNextMouseMove_ = false;
};
} // namespace engine
