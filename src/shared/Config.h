// TaskbarTuner - shared configuration contract between the host and the hook.
#pragma once

#include <windows.h>

namespace tuner {

// Registry location. The layout is compatible with the original TaskbarTuner
// so that an existing configuration keeps working after the rewrite.
constexpr wchar_t kRegKeyRoot[] = L"Software\\TaskbarTuner";
constexpr wchar_t kRunKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"TaskbarTuner";

// The hook creates this hidden top-level window inside explorer.exe. The host
// looks it up to find out whether the hook is live and to push changes.
constexpr wchar_t kHookWindowClass[] = L"TaskbarTunerHookWnd";
constexpr wchar_t kHookWindowTitle[] = L"TaskbarTunerHook";

constexpr wchar_t kHostWindowClass[] = L"TaskbarTunerHostWnd";

// Host -> hook.
constexpr UINT kMsgSettingsChanged = WM_APP + 0x51;
constexpr UINT kMsgShutdown = WM_APP + 0x52;      // restore and unload
constexpr UINT kMsgSuspend = WM_APP + 0x53;       // restore, stay loaded

// Hook -> host.
constexpr UINT kMsgHookReady = WM_APP + 0x61;

// Update the layout version when the meaning of a value changes. The original
// program wrote 1166 while its UI said v1.4.6 and its resources said 1.1.5;
// here a single number is used everywhere (see res/version.h).
//
// 3: Brightness became 0..100 with 100 = unchanged, matching the UI. Versions
// 1 and 2 stored 0..-100 with 0 = unchanged, and are converted on load.
constexpr int kConfigVersion = 3;

// Every value is shown on a 0..100 scale, left to right, exactly like the
// original program: 100 means "no change".
struct Config {
    int opacity = 100;       // background opacity, 100 = unchanged
    int brightness = 100;    // background brightness, 100 = unchanged, 0 = black
    int iconOpacity = 100;   // icon and text layer opacity, 100 = unchanged
    bool topLine = true;     // show the thin line at the top edge
    bool hideGrip = false;   // hide the drag gripper handle
    bool showAllTray = false;
    bool autoStart = false;
    bool enabled = true;

    bool IsNeutral() const {
        return opacity == 100 && brightness == 100 && iconOpacity == 100 &&
               topLine && !hideGrip;
    }
};

Config LoadConfig();
bool SaveConfig(const Config& config);

// Writes the configuration and asks a running hook to pick it up. Returns
// false when the hook is not loaded (the values are still saved).
bool PushConfig(const Config& config);

// Locates the hook window inside explorer.exe.
HWND FindHookWindow();

// Puts the taskbar back to its original appearance without changing what is
// stored. Used when the program exits: the look should be restored, but the
// user's settings must survive so that the next start applies them again.
void RequestHookSuspend();
void RequestHookShutdown();

// Restores the startup entry to point at the current executable (or removes it
// when autoStart is false).
void SetAutoStart(bool enabled);

// "Display all tray icons": flips IsPromoted for every notification icon and
// remembers the original values so that it can be undone exactly.
bool SetShowAllTrayIcons(bool enabled, const Config& config);

}  // namespace tuner
