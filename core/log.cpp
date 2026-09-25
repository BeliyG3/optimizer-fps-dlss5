#include "core/log.h"
#include <cstdarg>
#include <cstdio>

namespace ofps::core {
namespace {
LogFn g_log = nullptr;
} // namespace

void SetLog(LogFn log) { g_log = log; }

void Log(bool warning, const char *fmt, ...)
{
    if (g_log == nullptr) return;
    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    g_log(warning, buffer);
}

} // namespace ofps::core
