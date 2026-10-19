#include "Log.h"

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>

namespace tuner {
namespace {

CRITICAL_SECTION g_lock;
std::once_flag g_lockOnce;
HANDLE g_file = INVALID_HANDLE_VALUE;
std::wstring g_component;

// Both the host and the hook log, and the hook does it from a worker thread
// that starts at the same time as the one that injects it, so the lock has to
// be initialised exactly once no matter which thread gets there first.
CRITICAL_SECTION& Lock() {
    std::call_once(g_lockOnce, [] { InitializeCriticalSection(&g_lock); });
    return g_lock;
}

std::wstring LogPath() {
    wchar_t base[MAX_PATH * 2]{};
    DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", base,
                                           ARRAYSIZE(base));
    std::wstring dir;
    if (length == 0 || length >= ARRAYSIZE(base)) {
        wchar_t temp[MAX_PATH * 2]{};
        GetTempPathW(ARRAYSIZE(temp), temp);
        dir = temp;
    } else {
        dir = base;
    }

    dir += L"\\TaskbarTuner";
    CreateDirectoryW(dir.c_str(), nullptr);

    return dir + L"\\TaskbarTuner.log";
}

// The log is rotated by hand at startup so that it cannot grow without bound
// in a process (explorer.exe) that is never restarted.
void RotateIfLarge(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
        return;
    }

    ULARGE_INTEGER size;
    size.LowPart = data.nFileSizeLow;
    size.HighPart = data.nFileSizeHigh;
    if (size.QuadPart < 1024ull * 1024ull) {
        return;
    }

    std::wstring oldPath = path + L".old";
    DeleteFileW(oldPath.c_str());
    MoveFileExW(path.c_str(), oldPath.c_str(), MOVEFILE_REPLACE_EXISTING);
}

}  // namespace

void LogInit(const wchar_t* component) {
    Lock();

    std::wstring path = LogPath();
    RotateIfLarge(path);

    g_component = component ? component : L"?";
    g_file = CreateFileW(path.c_str(), FILE_APPEND_DATA,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                         OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

void LogShutdown() {
    EnterCriticalSection(&Lock());
    if (g_file != INVALID_HANDLE_VALUE) {
        CloseHandle(g_file);
        g_file = INVALID_HANDLE_VALUE;
    }
    LeaveCriticalSection(&Lock());
}

void LogWrite(const wchar_t* format, ...) {
    if (g_file == INVALID_HANDLE_VALUE) {
        return;
    }

    wchar_t message[2048]{};
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(message, _TRUNCATE, format, args);
    va_end(args);

    SYSTEMTIME now{};
    GetLocalTime(&now);

    wchar_t line[2304]{};
    _snwprintf_s(line, _TRUNCATE,
                 L"%04u-%02u-%02u %02u:%02u:%02u.%03u [%5lu] %-7s %s\r\n",
                 now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
                 now.wSecond, now.wMilliseconds, GetCurrentThreadId(),
                 g_component.c_str(), message);

    int chars = static_cast<int>(wcslen(line));
    int bytes = WideCharToMultiByte(CP_UTF8, 0, line, chars, nullptr, 0, nullptr,
                                    nullptr);
    if (bytes <= 0) {
        return;
    }

    std::string utf8(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, line, chars, utf8.data(), bytes, nullptr,
                        nullptr);

    EnterCriticalSection(&Lock());
    if (g_file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(g_file, utf8.data(), static_cast<DWORD>(utf8.size()), &written,
                  nullptr);
    }
    LeaveCriticalSection(&Lock());
}

}  // namespace tuner
