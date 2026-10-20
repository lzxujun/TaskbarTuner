// Hook DLL: loaded into explorer.exe by the host.
//
// It owns one hidden window that the host talks to, a worker thread that keeps
// the configuration in sync, and it re-applies the settings whenever Windows
// rebuilds the taskbar (theme switch, accent color change, explorer restart).
#include <windows.h>

#include <commctrl.h>

#include <atomic>
#include <algorithm>
#include <string>
#include <vector>

#include "Symbols.h"
#include "TaskbarHost.h"
#include "VisualTree.h"

#include "../shared/Config.h"
#include "../shared/Log.h"

namespace {

constexpr UINT_PTR kPollTimer = 0xA17F;
constexpr UINT kPollIntervalMs = 500;
constexpr ULONGLONG kRetryDelayMs = 1200;
constexpr ULONGLONG kSlowRetryDelayMs = 30000;
constexpr ULONGLONG kVerifyIntervalMs = 5000;

HINSTANCE g_instance = nullptr;
HWND g_window = nullptr;
UINT g_runFromWindowThreadMsg = 0;
std::atomic<bool> g_running{true};
// Set when the host asked for a real unload rather than just a restore.
bool g_unload = false;
tuner::Config g_config;

// A pass lives on the heap, not on the worker thread's stack: the taskbar thread
// may still be running it after SendMessageTimeout has given up waiting (a slow
// first probe easily exceeds the timeout), and a stack object would be gone by
// then while the taskbar thread kept writing to it.
struct ApplyPass {
    HWND hwnd = nullptr;
    bool secondary = false;
    tuner::Config config;
    std::atomic<tuner::hook::ApplyOutcome> outcome{
        tuner::hook::ApplyOutcome::Failed};

    void AddRef() { m_refs.fetch_add(1); }

    void Release() {
        if (m_refs.fetch_sub(1) == 1) {
            delete this;
        }
    }

private:
    std::atomic<int> m_refs{1};
};

struct TrackedTaskbar {
    HWND hwnd = nullptr;
    bool secondary = false;
    tuner::hook::ApplyOutcome lastOutcome = tuner::hook::ApplyOutcome::Failed;
    int failures = 0;
    ULONGLONG lastAttempt = 0;
    ULONGLONG lastVerify = 0;
};

struct TaskbarInfo {
    HWND hwnd;
    bool secondary;
};

std::vector<TrackedTaskbar> g_tracked;

void EnumTaskbars(std::vector<TaskbarInfo>& out) {
    EnumWindows(
        [](HWND hWnd, LPARAM lParam) -> BOOL {
            wchar_t className[64]{};
            if (!GetClassNameW(hWnd, className, ARRAYSIZE(className))) {
                return TRUE;
            }

            const bool secondary =
                _wcsicmp(className, L"Shell_SecondaryTrayWnd") == 0;
            if (!secondary && _wcsicmp(className, L"Shell_TrayWnd") != 0) {
                return TRUE;
            }

            reinterpret_cast<std::vector<TaskbarInfo>*>(lParam)
                ->push_back({hWnd, secondary});
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&out));
}

// Runs on the taskbar UI thread. XAML objects may only be touched there, which
// is also why every read of the tree happens in this callback and never on the
// worker thread.
void RunApplyPass(ApplyPass* pass) {
    using tuner::hook::ApplyOutcome;

    auto root = pass->secondary
                    ? tuner::hook::GetSecondaryTaskbarXamlRoot(pass->hwnd)
                    : tuner::hook::GetTaskbarXamlRoot(pass->hwnd);
    if (!root) {
        pass->outcome = ApplyOutcome::NoBackground;
        return;
    }

    // Already adjusted and nothing was torn down: leave it alone. This makes
    // the periodic verification free in the common case and, more importantly,
    // keeps it from repainting the taskbar every few seconds.
    if (tuner::hook::IsRootTrackedAndAlive(root, pass->config)) {
        pass->outcome = ApplyOutcome::Applied;
        return;
    }

    pass->outcome = tuner::hook::ApplyConfig(root, pass->config);
}

// The pass the taskbar thread has been handed, if any. Ownership moves through
// an exchange: whoever takes the pointer out of the slot also owns one
// reference, so the worker thread can safely walk away after a timeout.
std::atomic<ApplyPass*> g_pendingPass{nullptr};

// Set while a handoff may still be running on the taskbar thread. A second pass
// is not started while this is set: if the taskbar thread is too busy to answer,
// queueing more work on it only makes things worse.
std::atomic<bool> g_passInFlight{false};

// A thread-local hook is enough to get onto the taskbar's UI thread: the hook
// procedure is called in the context of the thread that owns the window, and
// because the DLL already lives in explorer.exe there is no out-of-process
// marshalling involved. Subclassing the window was the obvious alternative, but
// comctl32 refuses to subclass the shell's taskbar window, so the work is
// dispatched this way instead.
LRESULT CALLBACK CallWndProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION) {
        const auto* cwp = reinterpret_cast<const CWPSTRUCT*>(lParam);
        if (cwp->message == g_runFromWindowThreadMsg) {
            // The exchange makes this the only taker: the worker thread can
            // never free the pass underneath the apply that is still running.
            ApplyPass* pass = g_pendingPass.exchange(nullptr);
            if (pass) {
                try {
                    RunApplyPass(pass);
                } catch (...) {
                    TLOG(L"apply pass threw");
                    pass->outcome = tuner::hook::ApplyOutcome::Failed;
                }
                g_passInFlight = false;
                pass->Release();
            }
        }
    }

    return CallNextHookEx(nullptr, code, wParam, lParam);
}

bool RunFromWindowThread(HWND hWnd, ApplyPass* pass) {
    const DWORD thread = GetWindowThreadProcessId(hWnd, nullptr);
    if (!thread) {
        return false;
    }

    // One reference for the pending slot, one for the caller.
    pass->AddRef();
    g_pendingPass.store(pass);
    g_passInFlight = true;

    HHOOK hook = SetWindowsHookExW(WH_CALLWNDPROC, CallWndProc, nullptr, thread);
    if (!hook) {
        TLOG(L"SetWindowsHookEx failed for thread %lu (error %lu)", thread,
             GetLastError());
        if (g_pendingPass.exchange(nullptr) == pass) {
            pass->Release();
        }
        g_passInFlight = false;
        return false;
    }

    DWORD_PTR result = 0;
    const LRESULT sent = SendMessageTimeoutW(
        hWnd, g_runFromWindowThreadMsg, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK,
        5000, &result);

    UnhookWindowsHookEx(hook);

    // If the pointer is still in the slot nobody took it, so the pending
    // reference is still ours. Otherwise the taskbar thread owns it and will
    // release it when it finishes - even if that happens after this return.
    if (g_pendingPass.exchange(nullptr) == pass) {
        g_passInFlight = false;
        pass->Release();
    }

    if (!sent) {
        TLOG(L"SendMessageTimeout failed for %p (error %lu); the apply may still "
             L"be running on the taskbar thread",
             hWnd, GetLastError());
    }

    return sent != 0;
}

void ApplyToTaskbar(HWND hwnd, bool secondary) {
    if (g_passInFlight) {
        TLOG(L"previous apply on %p has not finished, skipping", hwnd);
        return;
    }

    auto* pass = new ApplyPass();
    pass->hwnd = hwnd;
    pass->secondary = secondary;
    pass->config = g_config;

    if (!RunFromWindowThread(hwnd, pass)) {
        TLOG(L"could not reach the taskbar thread of %p", hwnd);
        if (!g_passInFlight) {
            pass->outcome = tuner::hook::ApplyOutcome::Failed;
        }
    }

    // Read the outcome before dropping the last reference: after that the pass
    // may already belong to the taskbar thread and be gone.
    const auto outcome = pass->outcome.load();
    pass->Release();

    switch (outcome) {
        case tuner::hook::ApplyOutcome::Applied:
            TLOG(L"applied to %p", hwnd);
            break;
        case tuner::hook::ApplyOutcome::Restored:
            TLOG(L"restored %p", hwnd);
            break;
        case tuner::hook::ApplyOutcome::NoBackground:
            TLOG(L"taskbar %p is not laid out yet", hwnd);
            break;
        case tuner::hook::ApplyOutcome::NotReady:
            TLOG(L"icon layer of %p is not ready yet", hwnd);
            break;
        case tuner::hook::ApplyOutcome::Failed:
            TLOG(L"apply failed for %p", hwnd);
            break;
    }

    for (auto& entry : g_tracked) {
        if (entry.hwnd == hwnd) {
            // "Restored" is a success as well: it means the taskbar was
            // reached and put back, not that a probe failed.
            const bool success = outcome == tuner::hook::ApplyOutcome::Applied ||
                                 outcome == tuner::hook::ApplyOutcome::Restored;
            entry.lastOutcome = outcome;
            entry.failures = success ? 0 : entry.failures + 1;
            entry.lastAttempt = GetTickCount64();
            entry.lastVerify = GetTickCount64();
            return;
        }
    }

    TrackedTaskbar entry;
    entry.hwnd = hwnd;
    entry.secondary = secondary;
    entry.lastOutcome = outcome;
    entry.failures = (outcome == tuner::hook::ApplyOutcome::Applied ||
                      outcome == tuner::hook::ApplyOutcome::Restored)
                         ? 0
                         : 1;
    entry.lastAttempt = GetTickCount64();
    entry.lastVerify = GetTickCount64();
    g_tracked.push_back(entry);
}

void ApplyToAllTaskbars() {
    std::vector<TaskbarInfo> taskbars;
    EnumTaskbars(taskbars);

    for (auto const& info : taskbars) {
        ApplyToTaskbar(info.hwnd, info.secondary);
    }
}

// Keeps the list in sync with the taskbars that actually exist and retries the
// ones that were not ready, e.g. because the taskbar was still being created
// when the hook was injected.
void PollTaskbars() {
    // Paused, or every value is at its neutral setting: there is nothing to
    // re-apply, and probing a taskbar that is deliberately left alone would
    // only write log lines.
    if (!g_config.enabled || g_config.IsNeutral()) {
        return;
    }

    std::vector<TaskbarInfo> alive;
    EnumTaskbars(alive);

    for (auto it = g_tracked.begin(); it != g_tracked.end();) {
        const bool stillThere =
            std::any_of(alive.begin(), alive.end(), [&](TaskbarInfo const& info) {
                return info.hwnd == it->hwnd;
            });
        it = stillThere ? it + 1 : g_tracked.erase(it);
    }

    const ULONGLONG now = GetTickCount64();

    for (auto const& info : alive) {
        const bool known =
            std::any_of(g_tracked.begin(), g_tracked.end(),
                        [&](TrackedTaskbar const& entry) {
                            return entry.hwnd == info.hwnd;
                        });

        if (!known) {
            ApplyToTaskbar(info.hwnd, info.secondary);
            continue;
        }

        for (auto& entry : g_tracked) {
            if (entry.hwnd != info.hwnd) {
                continue;
            }

            const bool retryable =
                entry.lastOutcome == tuner::hook::ApplyOutcome::NoBackground ||
                entry.lastOutcome == tuner::hook::ApplyOutcome::NotReady ||
                entry.lastOutcome == tuner::hook::ApplyOutcome::Failed;

            // A probe that keeps failing (unsupported build, a taskbar whose
            // layout this program cannot resolve) is retried rarely instead of
            // every second: each attempt walks thousands of candidate pointers.
            const ULONGLONG retryDelay =
                entry.failures >= 5 ? kSlowRetryDelayMs : kRetryDelayMs;

            const bool shouldRetry =
                retryable && now - entry.lastAttempt >= retryDelay;
            const bool shouldVerify = now - entry.lastVerify >= kVerifyIntervalMs;

            if (shouldRetry || shouldVerify) {
                ApplyToTaskbar(info.hwnd, info.secondary);
            }
            break;
        }
    }
}

LRESULT CALLBACK HookWindowProc(HWND hWnd, UINT message, WPARAM wParam,
                                LPARAM lParam) {
    switch (message) {
        case WM_TIMER:
            if (wParam == kPollTimer) {
                PollTaskbars();
            }
            return 0;

        default:
            break;
    }

    if (message == tuner::kMsgSettingsChanged) {
        TLOG(L"settings changed, reloading");
        g_config = tuner::LoadConfig();
        // No probe cache reset here: the taskband object is the same one, and a
        // full rescan costs thousands of validated pointer reads. The probe
        // rescans on its own when the cached layout stops resolving.
        ApplyToAllTaskbars();
        return 0;
    }

    if (message == tuner::kMsgShutdown) {
        // Unload for real: the host sends this when it is going away for good,
        // so a later start injects the current build instead of finding a stale
        // copy of an older one still resident in explorer.
        TLOG(L"shutdown requested");
        g_unload = true;
        PostQuitMessage(0);
        return 0;
    }

    if (message == tuner::kMsgSuspend) {
        TLOG(L"restore requested");
        g_config = tuner::LoadConfig();
        g_config.enabled = false;
        ApplyToAllTaskbars();
        return 0;
    }

    if (message == WM_ENDSESSION) {
        g_running = false;
        return 0;
    }

    return DefWindowProcW(hWnd, message, wParam, lParam);
}

bool CreateHookWindow() {
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = HookWindowProc;
    windowClass.hInstance = g_instance;
    windowClass.lpszClassName = tuner::kHookWindowClass;

    if (!RegisterClassExW(&windowClass) &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        TLOG(L"RegisterClassEx failed: %lu", GetLastError());
        return false;
    }

    // A normal (never shown) top-level window, not a message-only window:
    // FindWindow does not search the message-only tree, and the host depends on
    // finding this window to know that the hook is alive.
    g_window = CreateWindowExW(0, tuner::kHookWindowClass, tuner::kHookWindowTitle,
                               0, 0, 0, 0, 0, nullptr, nullptr, g_instance,
                               nullptr);
    if (!g_window) {
        TLOG(L"CreateWindowEx failed: %lu", GetLastError());
        return false;
    }

    return true;
}

DWORD WINAPI WorkerThread(void* parameter) {
    (void)parameter;

    tuner::LogInit(L"hook");
    TLOG(L"hook attached to pid %lu", GetCurrentProcessId());

    // Resolved here, on this thread, on purpose: resolution may download the
    // PDB and must never run on the taskbar's UI thread.
    {
        std::wstring symbolError;
        if (tuner::hook::EnsureTaskbarSymbols(symbolError)) {
            TLOG(L"symbols loaded from %s",
                 tuner::hook::LoadedPdbPath().c_str());
        } else {
            TLOG(L"taskbar symbols unavailable: %s", symbolError.c_str());
        }
    }

    g_runFromWindowThreadMsg =
        RegisterWindowMessageW(L"TaskbarTuner_RunFromWindowThread");

    if (!CreateHookWindow()) {
        tuner::LogShutdown();
        return 1;
    }

    g_config = tuner::LoadConfig();

    // Give explorer a moment: when the DLL is injected right after logon the
    // taskbar may not exist yet, and the poll below would otherwise hammer the
    // probe for every tick.
    for (int i = 0; i < 20 && g_running; i++) {
        std::vector<TaskbarInfo> taskbars;
        EnumTaskbars(taskbars);
        if (!taskbars.empty()) {
            break;
        }
        Sleep(100);
    }

    ApplyToAllTaskbars();
    SetTimer(g_window, kPollTimer, kPollIntervalMs, nullptr);

    MSG message{};
    while (g_running && GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    KillTimer(g_window, kPollTimer);

    // Restore before going away so that explorer is never left with a brush
    // from module code that is about to be unmapped.
    g_config.enabled = false;
    ApplyToAllTaskbars();

    if (g_window) {
        DestroyWindow(g_window);
        g_window = nullptr;
    }

    TLOG(L"hook detached");
    tuner::LogShutdown();

    if (g_unload) {
        // Restoring released every brush and tree reference above; give the
        // taskbar thread a beat to finish its own work before the module goes
        // away, because a CompositionBrush created from this DLL keeps a
        // reference to a class implemented here.
        Sleep(300);
        FreeLibraryAndExitThread(g_instance, 0);
    }

    return 0;
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)reserved;

    switch (reason) {
        case DLL_PROCESS_ATTACH: {
            g_instance = instance;
            DisableThreadLibraryCalls(instance);

            // Nothing that touches the loader may run here, hence the thread.
            HANDLE thread =
                CreateThread(nullptr, 0, WorkerThread, nullptr, 0, nullptr);
            if (thread) {
                CloseHandle(thread);
            }
            break;
        }

        case DLL_PROCESS_DETACH:
            g_running = false;
            break;

        default:
            break;
    }

    return TRUE;
}
