#include "Injector.h"

#include "../shared/Config.h"
#include "../shared/Log.h"

#include "../../res/resource.h"

#include <cstdio>
#include <cstring>
#include <cstdint>

namespace tuner {
namespace {

std::wstring LocalAppDataDirectory() {
    wchar_t base[MAX_PATH * 2]{};
    const DWORD length =
        GetEnvironmentVariableW(L"LOCALAPPDATA", base, ARRAYSIZE(base));

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
    return dir;
}

uint64_t Fnv1a(const void* data, size_t size) {
    auto* bytes = static_cast<const uint8_t*>(data);
    uint64_t hash = 1469598103934665603ull;
    for (size_t i = 0; i < size; i++) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

bool FileMatches(const std::wstring& path, const void* data, size_t size) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    LARGE_INTEGER fileSize{};
    const bool readable = GetFileSizeEx(file, &fileSize) != 0;
    if (!readable || static_cast<size_t>(fileSize.QuadPart) != size) {
        CloseHandle(file);
        return false;
    }

    // Only the size and a hash of the head are compared: the payload is a
    // build output, so a partial match is impossible in practice and this avoids
    // reading a megabyte on every start.
    constexpr DWORD kProbeBytes = 4096;
    BYTE buffer[kProbeBytes]{};
    DWORD read = 0;
    const bool ok = ReadFile(file, buffer, kProbeBytes, &read, nullptr) != 0;
    CloseHandle(file);

    if (!ok || read == 0) {
        return false;
    }

    return memcmp(buffer, data, read) == 0;
}

bool WriteAll(const std::wstring& path, const void* data, size_t size) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    DWORD written = 0;
    const bool ok =
        WriteFile(file, data, static_cast<DWORD>(size), &written, nullptr) != 0 &&
        written == size;
    CloseHandle(file);

    if (!ok) {
        DeleteFileW(path.c_str());
    }

    return ok;
}

}  // namespace

bool ExtractHookDll(std::wstring& path, std::wstring& error) {
    HRSRC resource =
        FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_HOOK_DLL), RT_RCDATA);
    if (!resource) {
        error = L"程序内没有找到注入模块（构建时资源未打包）";
        return false;
    }

    const DWORD size = SizeofResource(nullptr, resource);
    HGLOBAL handle = LoadResource(nullptr, resource);
    const void* data = handle ? LockResource(handle) : nullptr;
    if (!data || size == 0) {
        error = L"注入模块资源损坏";
        return false;
    }

    const std::wstring directory = LocalAppDataDirectory();
    path = directory + L"\\TaskbarTunerHook.dll";

    if (FileMatches(path, data, size)) {
        return true;
    }

    const std::wstring temporary = path + L".tmp";
    if (WriteAll(temporary, data, size) &&
        MoveFileExW(temporary.c_str(), path.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        TLOG(L"hook dll extracted to %s", path.c_str());
        return true;
    }

    DeleteFileW(temporary.c_str());

    // The old file is locked by a running instance. A content-addressed name
    // lets the new build sit next to it until explorer restarts.
    wchar_t suffix[32]{};
    swprintf_s(suffix, L"%016llX", static_cast<unsigned long long>(Fnv1a(data, size)));
    path = directory + L"\\TaskbarTunerHook_" + suffix + L".dll";

    if (WriteAll(path, data, size)) {
        TLOG(L"hook dll written to alternate path %s", path.c_str());
        return true;
    }

    error = L"无法写入注入模块（" + path + L"）";
    return false;
}

bool InjectHook(HWND hTaskbarWnd, const std::wstring& dllPath,
                std::wstring& error) {
    DWORD processId = 0;
    GetWindowThreadProcessId(hTaskbarWnd, &processId);
    if (!processId) {
        error = L"无法确定任务栏所属进程";
        return false;
    }

    TLOG(L"injecting %s into pid %lu", dllPath.c_str(), processId);

    HANDLE process =
        OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                        PROCESS_VM_OPERATION | PROCESS_VM_WRITE,
                    FALSE, processId);
    if (!process) {
        error = L"无法打开 explorer 进程（错误 " +
                std::to_wstring(GetLastError()) +
                L"）。如果任务栏属于其他用户或受保护进程，请以相同权限运行本程序。";
        return false;
    }

    const size_t bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(process, nullptr, bytes,
                                  MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) {
        error = L"无法在 explorer 中分配内存（错误 " +
                std::to_wstring(GetLastError()) + L"）";
        CloseHandle(process);
        return false;
    }

    bool ok = false;

    if (WriteProcessMemory(process, remote, dllPath.c_str(), bytes, nullptr)) {
        HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
        auto loadLibrary = kernel32
                               ? reinterpret_cast<LPTHREAD_START_ROUTINE>(
                                     GetProcAddress(kernel32, "LoadLibraryW"))
                               : nullptr;

        if (loadLibrary) {
            HANDLE thread =
                CreateRemoteThread(process, nullptr, 0, loadLibrary, remote, 0,
                                   nullptr);
            if (thread) {
                WaitForSingleObject(thread, 15000);
                CloseHandle(thread);
                ok = true;
            } else {
                error = L"无法在 explorer 中创建线程（错误 " +
                        std::to_wstring(GetLastError()) + L"）";
            }
        } else {
            error = L"无法定位 LoadLibraryW";
        }
    } else {
        error = L"无法写入 explorer 内存（错误 " +
                std::to_wstring(GetLastError()) + L"）";
    }

    VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    CloseHandle(process);

    return ok;
}

HWND FindPrimaryTaskbar() {
    return FindWindowW(L"Shell_TrayWnd", nullptr);
}

bool IsHookLoaded() {
    return FindHookWindow() != nullptr;
}

bool EnsureHookLoaded(std::wstring& error) {
    if (IsHookLoaded()) {
        TLOG(L"hook already loaded");
        return true;
    }

    std::wstring dllPath;
    if (!ExtractHookDll(dllPath, error)) {
        return false;
    }

    HWND taskbar = FindPrimaryTaskbar();
    if (!taskbar) {
        error = L"找不到任务栏窗口";
        return false;
    }

    if (!InjectHook(taskbar, dllPath, error)) {
        return false;
    }

    for (int i = 0; i < 120; i++) {
        if (IsHookLoaded()) {
            TLOG(L"hook window appeared after %d ms", i * 50);
            return true;
        }
        Sleep(50);
    }

    error =
        L"注入已完成，但注入模块没有初始化。请查看日志 "
        L"%LOCALAPPDATA%\\TaskbarTuner\\TaskbarTuner.log。";
    return false;
}

}  // namespace tuner
