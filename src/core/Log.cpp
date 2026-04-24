#include "core/Log.h"

#include <Windows.h>

#include <iostream>
#include <mutex>
#include <string>

namespace engine
{
namespace
{
std::mutex gLogMutex;

const char* PrefixForLevel(LogLevel level)
{
    switch (level)
    {
    case LogLevel::Info:
        return "[Info] ";
    case LogLevel::Warning:
        return "[Warn] ";
    case LogLevel::Error:
        return "[Error]";
    default:
        return "[Log] ";
    }
}
} // namespace

void InitializeLogging()
{
    static bool initialized = false;
    if (initialized)
    {
        return;
    }

    initialized = true;

    if (::AllocConsole() || ::GetLastError() == ERROR_ACCESS_DENIED)
    {
        FILE* stream = nullptr;
        freopen_s(&stream, "CONOUT$", "w", stdout);
        freopen_s(&stream, "CONOUT$", "w", stderr);
        freopen_s(&stream, "CONIN$", "r", stdin);
        std::ios::sync_with_stdio(true);
    }
}

void Log(LogLevel level, std::string_view message)
{
    std::lock_guard<std::mutex> lock(gLogMutex);

    const std::string line = std::string(PrefixForLevel(level)) + " " + std::string(message) + "\n";
    ::OutputDebugStringA(line.c_str());
    std::cout << line;
    std::cout.flush();
}
} // namespace engine
