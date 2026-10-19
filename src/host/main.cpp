// TaskbarTuner host: tray icon, settings panel and the injection lifecycle.
#include <windows.h>

#include <shellapi.h>
#include <commctrl.h>

#include <cstdio>
#include <string>
#include <memory>

#include "Injector.h"
#include "Panel.h"

#include "../../res/resource.h"
#include "../../res/version.h"
#include "../shared/Config.h"
#include "../shared/Log.h"

namespace {

constexpr wchar_t kAppName[] = L"任务栏调节器";
constexpr wchar_t kMutexName[] = L"TaskbarTuner_SingleInstance_Mutex";

// Same numbers as the version resource: res/version.h is the only place that
// holds them.
std::wstring AppVersion() {
    wchar_t buffer[32]{};
    swprintf_s(buffer, L"%d.%d.%d", TUNER_VERSION_MAJOR, TUNER_VERSION_MINOR,
               TUNER_VERSION_PATCH);
    return buffer;
}

constexpr UINT kTrayMessage = WM_APP + 0x01;
constexpr UINT kShowPanelMessage = WM_APP + 0x02;

constexpr UINT kMenuOpen = 1001;
constexpr UINT kMenuEnabled = 1002;
constexpr UINT kMenuRestore = 1003;
constexpr UINT kMenuAutoStart = 1004;
constexpr UINT kMenuAbout = 1005;
constexpr UINT kMenuExit = 1006;

HINSTANCE g_instance = nullptr;
HWND g_hostWindow = nullptr;
tuner::Panel* g_panel = nullptr;
NOTIFYICONDATAW g_trayIcon{};
UINT g_taskbarCreatedMessage = 0;

tuner::Config g_config;
bool g_lastShowAllTray = false;
bool g_hookReady = false;

void UpdateTrayTip() {
    wcscpy_s(g_trayIcon.szTip,
             g_hookReady ? L"任务栏调节器 - 已生效" : L"任务栏调节器 - 未连接");
    Shell_NotifyIconW(NIM_MODIFY, &g_trayIcon);
}

void AddTrayIcon() {
    g_trayIcon.cbSize = sizeof(g_trayIcon);
    g_trayIcon.hWnd = g_hostWindow;
    g_trayIcon.uID = 1;
    g_trayIcon.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_trayIcon.uCallbackMessage = kTrayMessage;
    g_trayIcon.hIcon = static_cast<HICON>(
        LoadImageW(g_instance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
                   GetSystemMetrics(SM_CXSMICON),
                   GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    wcscpy_s(g_trayIcon.szTip, L"任务栏调节器");

    Shell_NotifyIconW(NIM_ADD, &g_trayIcon);
}

void RemoveTrayIcon() {
    Shell_NotifyIconW(NIM_DELETE, &g_trayIcon);
}

// Makes sure the hook lives in explorer and holds the current configuration.
void SyncToHook() {
    if (!g_panel) {
        return;
    }

    if (!tuner::IsHookLoaded()) {
        std::wstring error;
        if (!tuner::EnsureHookLoaded(error)) {
            TLOG(L"hook not available: %s", error.c_str());
            g_hookReady = false;
            g_panel->SetStatusText(error, true);
            UpdateTrayTip();
            return;
        }
    }

    tuner::PushConfig(g_config);
    g_hookReady = true;
    g_panel->SetStatusText(g_config.enabled ? L"已连接到任务栏" : L"效果已暂停",
                           false);
    UpdateTrayTip();
}

void OnPanelChanged(tuner::Config const& config) {
    const bool showAllChanged = config.showAllTray != g_lastShowAllTray;

    g_config = config;
    g_config.enabled = true;
    g_lastShowAllTray = config.showAllTray;

    if (showAllChanged) {
        if (!tuner::SetShowAllTrayIcons(config.showAllTray, config) &&
            config.showAllTray) {
            g_panel->SetStatusText(
                L"未找到托盘图标设置项（Control Panel\\NotifyIconSettings）。"
                L"该功能需要 Windows 11。",
                true);
        }
    }

    SyncToHook();
}

void RestoreDefaults() {
    tuner::Config defaults;
    defaults.autoStart = g_config.autoStart;
    defaults.showAllTray = false;

    if (g_lastShowAllTray) {
        tuner::SetShowAllTrayIcons(false, defaults);
        g_lastShowAllTray = false;
    }

    g_config = defaults;
    tuner::SaveConfig(g_config);

    if (g_panel) {
        g_panel->SetConfig(g_config);
    }

    SyncToHook();
}

// Puts the taskbar back and unloads. The values stay in the registry, so the
// next start restores the same look. Unloading matters beyond tidiness: a
// resident DLL keeps its file locked inside explorer.exe, which would stop the
// user from deleting the folder without restarting the shell.
void RequestExit() {
    tuner::RequestHookShutdown();
    if (g_lastShowAllTray) {
        tuner::Config defaults;
        tuner::SetShowAllTrayIcons(false, defaults);
    }
    if (g_hostWindow) {
        DestroyWindow(g_hostWindow);
    }
}

void ToggleEnabled() {
    g_config.enabled = !g_config.enabled;
    tuner::SaveConfig(g_config);

    if (g_config.enabled) {
        SyncToHook();
    } else if (tuner::IsHookLoaded()) {
        tuner::PushConfig(g_config);
        g_hookReady = false;
        if (g_panel) {
            g_panel->SetStatusText(L"效果已暂停", false);
        }
        UpdateTrayTip();
    }
}

void ShowAbout() {
    std::wstring text;
    text += L"任务栏调节器 v";
    text += AppVersion();
    text += L"\n\n";
    text += L"直接操作任务栏的 XAML 视觉树和合成层，调整不透明度、背景亮度、";
    text += L"图标层透明度与顶部细线。\n\n";
    text += L"本程序是 Windhawk 模块 taskbar-brightness-and-opacity-tuner";
    text += L"（作者 lzxujun，GPL-3.0）的独立重建版本，不依赖 Windhawk。\n\n";
    text += L"日志：%LOCALAPPDATA%\\TaskbarTuner\\TaskbarTuner.log";

    MessageBoxW(g_hostWindow, text.c_str(), L"关于",
                MB_OK | MB_ICONINFORMATION);
}

void ShowTrayMenu() {
    HMENU menu = CreatePopupMenu();
    if (!menu) {
        return;
    }

    AppendMenuW(menu, MF_STRING, kMenuOpen, L"打开设置面板");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (g_config.enabled ? MF_CHECKED : 0), kMenuEnabled,
                L"启用效果");
    AppendMenuW(menu, MF_STRING, kMenuRestore, L"恢复默认外观");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (g_config.autoStart ? MF_CHECKED : 0),
                kMenuAutoStart, L"开机自启动");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuAbout, L"关于");
    AppendMenuW(menu, MF_STRING, kMenuExit, L"退出");

    POINT cursor{};
    GetCursorPos(&cursor);

    // Without the foreground window the menu stays on screen after a click
    // somewhere else.
    SetForegroundWindow(g_hostWindow);

    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, cursor.x, cursor.y,
                   0, g_hostWindow, nullptr);
    DestroyMenu(menu);
    PostMessageW(g_hostWindow, WM_NULL, 0, 0);
}

void OnTrayMessage(WPARAM /*wParam*/, LPARAM lParam) {
    if (LOWORD(lParam) == WM_LBUTTONUP || LOWORD(lParam) == WM_LBUTTONDBLCLK) {
        if (g_panel) {
            g_panel->Toggle();
        }
        return;
    }

    if (LOWORD(lParam) == WM_RBUTTONUP) {
        ShowTrayMenu();
        return;
    }
}

void OnMenuCommand(UINT id) {
    switch (id) {
        case kMenuOpen:
            if (g_panel) {
                g_panel->Show();
            }
            break;

        case kMenuEnabled:
            ToggleEnabled();
            break;

        case kMenuRestore:
            RestoreDefaults();
            break;

        case kMenuAutoStart:
            g_config.autoStart = !g_config.autoStart;
            tuner::SaveConfig(g_config);
            tuner::SetAutoStart(g_config.autoStart);
            if (g_panel) {
                g_panel->SetConfig(g_config);
            }
            break;

        case kMenuAbout:
            ShowAbout();
            break;

        case kMenuExit:
            RequestExit();
            break;

        default:
            break;
    }
}

LRESULT CALLBACK HostWindowProc(HWND hwnd, UINT message, WPARAM wParam,
                                LPARAM lParam) {
    if (message == g_taskbarCreatedMessage && g_taskbarCreatedMessage != 0) {
        // Explorer restarted: the tray icon and the hook are both gone.
        TLOG(L"TaskbarCreated received, re-registering");
        g_hookReady = false;
        AddTrayIcon();
        SyncToHook();
        return 0;
    }

    switch (message) {
        case kTrayMessage:
            OnTrayMessage(wParam, lParam);
            return 0;

        case kShowPanelMessage:
            if (g_panel) {
                g_panel->Show();
            }
            return 0;

        case WM_COMMAND:
            OnMenuCommand(LOWORD(wParam));
            return 0;

        case WM_ENDSESSION:
            if (wParam && g_lastShowAllTray) {
                tuner::Config defaults;
                tuner::SetShowAllTrayIcons(false, defaults);
            }
            return 0;

        case WM_DESTROY:
            RemoveTrayIcon();
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int) {
    tuner::LogInit(L"host");

    // A second instance just brings the existing panel up.
    HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    const bool alreadyRunning = mutex && GetLastError() == ERROR_ALREADY_EXISTS;

    if (alreadyRunning) {
        if (HWND existing =
                FindWindowW(tuner::kHostWindowClass, nullptr)) {
            PostMessageW(existing, kShowPanelMessage, 0, 0);
        }
        return 0;
    }

    g_instance = instance;

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&controls);

    g_taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW hostClass{};
    hostClass.cbSize = sizeof(hostClass);
    hostClass.lpfnWndProc = HostWindowProc;
    hostClass.hInstance = instance;
    hostClass.lpszClassName = tuner::kHostWindowClass;

    if (!RegisterClassExW(&hostClass)) {
        TLOG(L"RegisterClassEx failed: %lu", GetLastError());
        return 1;
    }

    g_hostWindow = CreateWindowExW(0, tuner::kHostWindowClass, kAppName, 0, 0, 0,
                                   0, 0, nullptr, nullptr, instance, nullptr);
    if (!g_hostWindow) {
        TLOG(L"CreateWindowEx failed: %lu", GetLastError());
        return 1;
    }

    g_config = tuner::LoadConfig();
    g_lastShowAllTray = g_config.showAllTray;

    auto panel = std::make_unique<tuner::Panel>();
    if (!panel->Create(instance)) {
        TLOG(L"panel creation failed");
        return 1;
    }

    g_panel = panel.get();
    g_panel->SetConfig(g_config);
    g_panel->SetChangeHandler(&OnPanelChanged);
    g_panel->SetExitHandler(&RequestExit);

    AddTrayIcon();

    // ShowAllTray is a registry side effect in the shell's own key and does not
    // survive a session, so the stored preference is applied again here.
    if (g_config.showAllTray) {
        tuner::SetShowAllTrayIcons(true, g_config);
    }

    if (g_config.enabled) {
        SyncToHook();
    } else {
        g_panel->SetStatusText(L"效果已暂停", false);
    }

    tuner::SetAutoStart(g_config.autoStart);

    // "--tray" is used by the startup entry: no panel, just the tray icon.
    const bool silent =
        commandLine && wcsstr(commandLine, L"--tray") != nullptr;
    if (!silent) {
        g_panel->Show();
    }

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    g_panel = nullptr;
    tuner::LogShutdown();

    if (mutex) {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
    }

    return 0;
}
