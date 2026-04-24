#pragma once

#include <Windows.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace engine
{
class Window
{
public:
    struct MessageResult
    {
        bool handled = false;
        LRESULT result = 0;
    };

    using ResizeCallback = std::function<void(uint32_t width, uint32_t height, bool minimized)>;
    using MessageCallback = std::function<std::optional<MessageResult>(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)>;

    Window() = default;
    ~Window();

    bool Create(const wchar_t* title, uint32_t width, uint32_t height);
    bool PumpMessages();

    void SetResizeCallback(ResizeCallback callback);
    void SetMessageCallback(MessageCallback callback);
    void RequestClose();
    void SetTitle(const std::wstring& title);

    HWND Handle() const { return hwnd_; }
    uint32_t Width() const { return width_; }
    uint32_t Height() const { return height_; }
    const std::wstring& Title() const { return title_; }

private:
    static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    HWND hwnd_ = nullptr;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    std::wstring title_{};
    ResizeCallback resizeCallback_{};
    MessageCallback messageCallback_{};
};
} // namespace engine
