#pragma once

#include <string_view>

namespace engine
{
enum class LogLevel
{
    Info,
    Warning,
    Error
};

void InitializeLogging();
void Log(LogLevel level, std::string_view message);
} // namespace engine
