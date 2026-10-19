// Minimal thread-safe logger shared by the host and the hook. Both processes
// write to the same file, which is what makes an injection problem visible:
// the host logs the injection attempt, the hook logs what happened inside
// explorer.exe, and the two lines end up next to each other.
#pragma once

#include <windows.h>

namespace tuner {

void LogInit(const wchar_t* component);
void LogShutdown();
void LogWrite(const wchar_t* format, ...);

// Convenience macro that keeps the component name out of every call site.
#define TLOG(...) ::tuner::LogWrite(__VA_ARGS__)

}  // namespace tuner
