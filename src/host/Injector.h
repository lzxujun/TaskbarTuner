// Releases the embedded hook DLL next to the user's profile and injects it
// into the explorer process that owns the taskbar.
#pragma once

#include <windows.h>

#include <string>

namespace tuner {

// Extracts the hook DLL (RCDATA resource) to
// %LOCALAPPDATA%\TaskbarTuner\ and returns its path. The extraction is
// skipped when the file on disk is already identical, which also keeps it
// replaceable while explorer has it loaded by a previous session.
bool ExtractHookDll(std::wstring& path, std::wstring& error);

// Injects the DLL into the process that owns hTaskbarWnd.
bool InjectHook(HWND hTaskbarWnd, const std::wstring& dllPath,
                std::wstring& error);

// Finds the taskbar of the current session.
HWND FindPrimaryTaskbar();

// True when the hook window exists in explorer.
bool IsHookLoaded();

// Full flow: extract, inject and wait for the hook window to show up.
bool EnsureHookLoaded(std::wstring& error);

}  // namespace tuner
