#pragma once

#include <Windows.h>

namespace engine
{
class Timer
{
public:
    Timer()
    {
        ::QueryPerformanceFrequency(&frequency_);
        ::QueryPerformanceCounter(&lastCounter_);
    }

    float Tick()
    {
        LARGE_INTEGER currentCounter{};
        ::QueryPerformanceCounter(&currentCounter);
        const double elapsed = static_cast<double>(currentCounter.QuadPart - lastCounter_.QuadPart) /
            static_cast<double>(frequency_.QuadPart);
        lastCounter_ = currentCounter;
        return static_cast<float>(elapsed);
    }

private:
    LARGE_INTEGER frequency_{};
    LARGE_INTEGER lastCounter_{};
};
} // namespace engine
