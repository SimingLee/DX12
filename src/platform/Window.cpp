#include "platform/Window.h"

#include "core/Log.h"

namespace engine
{
namespace
{
constexpr const wchar_t* kWindowClassName = L"DX12MiniEngineWindowClass";
}

Window::~Window()
{
    if (hwnd_ != nullptr)
    {
        ::DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

bool Window::Create(const wchar_t* title, uint32_t width, uint32_t height)
{
    HINSTANCE instance = ::GetModuleHandleW(nullptr);

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = &Window::StaticWndProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = kWindowClassName;
    windowClass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;

    if (!::RegisterClassExW(&windowClass) && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        Log(LogLevel::Error, "Failed to register Win32 window class.");
        return false;
    }

    RECT clientRect{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    ::AdjustWindowRect(&clientRect, WS_OVERLAPPEDWINDOW, FALSE);

    hwnd_ = ::CreateWindowExW(
        0,
        kWindowClassName,
        title,
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        clientRect.right - clientRect.left,
        clientRect.bottom - clientRect.top,
        nullptr,
        nullptr,
        instance,
        this);

    if (hwnd_ == nullptr)
    {
        Log(LogLevel::Error, "Failed to create Win32 window.");
        return false;
    }

    width_ = width;
    height_ = height;
    title_ = title;

    ::ShowWindow(hwnd_, SW_SHOW);
    ::UpdateWindow(hwnd_);
    return true;
}

bool Window::PumpMessages()
{
    MSG message{};
    while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
    {
        if (message.message == WM_QUIT)
        {
            return false;
        }

        ::TranslateMessage(&message);
        ::DispatchMessageW(&message);
    }

    return true;
}

void Window::SetResizeCallback(ResizeCallback callback)
{
    resizeCallback_ = std::move(callback);
}

void Window::SetMessageCallback(MessageCallback callback)
{
    messageCallback_ = std::move(callback);
}

void Window::RequestClose()
{
    if (hwnd_ != nullptr)
    {
        ::PostMessageW(hwnd_, WM_CLOSE, 0, 0);
    }
}

void Window::SetTitle(const std::wstring& title)
{
    title_ = title;
    if (hwnd_ != nullptr)
    {
        ::SetWindowTextW(hwnd_, title_.c_str());
    }
}

LRESULT CALLBACK Window::StaticWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    Window* window = nullptr;

    if (message == WM_NCCREATE)
    {
        const auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
        window = reinterpret_cast<Window*>(createStruct->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));
        window->hwnd_ = hwnd;
    }
    else
    {
        window = reinterpret_cast<Window*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (window != nullptr)
    {
        return window->HandleMessage(message, wParam, lParam);
    }

    return ::DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT Window::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam)
{
    if (messageCallback_)
    {
        const std::optional<MessageResult> callbackResult = messageCallback_(hwnd_, message, wParam, lParam);
        if (callbackResult.has_value() && callbackResult->handled)
        {
            return callbackResult->result;
        }
    }

    switch (message)
    {
    case WM_SIZE:
    {
        width_ = static_cast<uint32_t>(LOWORD(lParam));
        height_ = static_cast<uint32_t>(HIWORD(lParam));
        const bool minimized = (wParam == SIZE_MINIMIZED);
        if (resizeCallback_)
        {
            resizeCallback_(width_, height_, minimized);
        }
        return 0;
    }
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    default:
        return ::DefWindowProcW(hwnd_, message, wParam, lParam);
    }
}
} // namespace engine
