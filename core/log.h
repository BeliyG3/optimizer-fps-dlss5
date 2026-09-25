#pragma once
namespace ofps::core { using LogFn = void (*)(bool warning, const char *message); void SetLog(LogFn log); void Log(bool warning, const char *fmt, ...); }
