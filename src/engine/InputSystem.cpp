#include "engine/InputSystem.h"

#include <windowsx.h>

namespace engine
{
void InputSystem::BeginFrame()
{
    previousKeys_ = currentKeys_;
    previousMouseButtons_ = currentMouseButtons_;
    mouseDelta_ = {};
}

void InputSystem::HandleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (wParam < currentKeys_.size())
        {
            currentKeys_[static_cast<size_t>(wParam)] = true;
        }
        break;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        if (wParam < currentKeys_.size())
        {
            currentKeys_[static_cast<size_t>(wParam)] = false;
        }
        break;
    case WM_LBUTTONDOWN:
        mousePosition_.x = static_cast<float>(GET_X_LPARAM(lParam));
        mousePosition_.y = static_cast<float>(GET_Y_LPARAM(lParam));
        currentMouseButtons_[0] = true;
        break;
    case WM_LBUTTONUP:
        mousePosition_.x = static_cast<float>(GET_X_LPARAM(lParam));
        mousePosition_.y = static_cast<float>(GET_Y_LPARAM(lParam));
        currentMouseButtons_[0] = false;
        break;
    case WM_RBUTTONDOWN:
        mousePosition_.x = static_cast<float>(GET_X_LPARAM(lParam));
        mousePosition_.y = static_cast<float>(GET_Y_LPARAM(lParam));
        currentMouseButtons_[1] = true;
        if (!mouseCaptured_)
        {
            CaptureMouse(hwnd, false);
        }
        break;
    case WM_RBUTTONUP:
        mousePosition_.x = static_cast<float>(GET_X_LPARAM(lParam));
        mousePosition_.y = static_cast<float>(GET_Y_LPARAM(lParam));
        currentMouseButtons_[1] = false;
        if (!forcedCapture_)
        {
            ReleaseMouseCapture(hwnd);
        }
        break;
    case WM_MBUTTONDOWN:
        mousePosition_.x = static_cast<float>(GET_X_LPARAM(lParam));
        mousePosition_.y = static_cast<float>(GET_Y_LPARAM(lParam));
        currentMouseButtons_[2] = true;
        break;
    case WM_MBUTTONUP:
        mousePosition_.x = static_cast<float>(GET_X_LPARAM(lParam));
        mousePosition_.y = static_cast<float>(GET_Y_LPARAM(lParam));
        currentMouseButtons_[2] = false;
        break;
    case WM_MOUSEMOVE:
        mousePosition_.x = static_cast<float>(GET_X_LPARAM(lParam));
        mousePosition_.y = static_cast<float>(GET_Y_LPARAM(lParam));
        if (mouseCaptured_)
        {
            if (ignoreNextMouseMove_)
            {
                ignoreNextMouseMove_ = false;
                break;
            }

            RECT rect{};
            ::GetClientRect(hwnd, &rect);
            const LONG centerX = (rect.right - rect.left) / 2;
            const LONG centerY = (rect.bottom - rect.top) / 2;
            const LONG x = GET_X_LPARAM(lParam);
            const LONG y = GET_Y_LPARAM(lParam);

            mouseDelta_.x += static_cast<float>(x - centerX);
            mouseDelta_.y += static_cast<float>(y - centerY);

            CenterCursor(hwnd);
        }
        break;
    case WM_KILLFOCUS:
        ClearState();
        ReleaseMouseCapture(hwnd);
        break;
    default:
        break;
    }
}

bool InputSystem::IsKeyDown(uint32_t key) const
{
    return key < currentKeys_.size() ? currentKeys_[key] : false;
}

bool InputSystem::WasKeyPressed(uint32_t key) const
{
    return key < currentKeys_.size() ? currentKeys_[key] && !previousKeys_[key] : false;
}

bool InputSystem::IsMouseButtonDown(uint32_t button) const
{
    return button < currentMouseButtons_.size() ? currentMouseButtons_[button] : false;
}

bool InputSystem::WasMouseButtonPressed(uint32_t button) const
{
    return button < currentMouseButtons_.size() ? currentMouseButtons_[button] && !previousMouseButtons_[button] : false;
}

MouseDelta InputSystem::GetMouseDelta() const
{
    return mouseDelta_;
}

MousePosition InputSystem::GetMousePosition() const
{
    return mousePosition_;
}

void InputSystem::ReleaseMouseCapture(HWND hwnd)
{
    if (!mouseCaptured_)
    {
        return;
    }

    mouseCaptured_ = false;
    forcedCapture_ = false;
    ::ReleaseCapture();
    ShowCapturedCursor(true);
    ignoreNextMouseMove_ = false;
    ::ClipCursor(nullptr);
    if (hwnd != nullptr)
    {
        ::SetCursor(::LoadCursorW(nullptr, IDC_ARROW));
    }
}

void InputSystem::CaptureMouse(HWND hwnd, bool forced)
{
    if (hwnd == nullptr || mouseCaptured_)
    {
        return;
    }

    mouseCaptured_ = true;
    forcedCapture_ = forced;
    ::SetCapture(hwnd);
    ShowCapturedCursor(false);
    CenterCursor(hwnd);
}

void InputSystem::CenterCursor(HWND hwnd)
{
    RECT rect{};
    ::GetClientRect(hwnd, &rect);
    POINT center{(rect.right - rect.left) / 2, (rect.bottom - rect.top) / 2};
    ::ClientToScreen(hwnd, &center);
    ignoreNextMouseMove_ = true;
    ::SetCursorPos(center.x, center.y);
}

void InputSystem::ShowCapturedCursor(bool visible)
{
    if (visible)
    {
        while (::ShowCursor(TRUE) < 0)
        {
        }
    }
    else
    {
        while (::ShowCursor(FALSE) >= 0)
        {
        }
    }
}

void InputSystem::ClearState()
{
    currentKeys_.fill(false);
    previousKeys_.fill(false);
    currentMouseButtons_.fill(false);
    previousMouseButtons_.fill(false);
    mouseDelta_ = {};
    mousePosition_ = {};
}
} // namespace engine
