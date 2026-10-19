// Applies the configuration to the taskbar XAML tree and restores it.
//
// The logic is a direct port of the Windhawk mod this program is based on
// (taskbar-brightness-and-opacity-tuner by lzxujun), with two changes: the
// settings come from the registry instead of Wh_GetIntSetting, and every
// modified value is tracked so that "restore" puts back exactly what Windows
// had, even if Windows rewrote the value in the meantime.
#pragma once

#include "../shared/Config.h"

#include <windows.h>

#include <winrt/Windows.UI.Xaml.h>

namespace tuner::hook {

enum class ApplyOutcome {
    Applied,        // the tree was adjusted
    Restored,       // neutral settings, original look put back
    NoBackground,   // BackgroundFill missing: taskbar not laid out yet
    NotReady,       // icon layer empty: retry later
    Failed,
};

// Must be called on the taskbar UI thread.
ApplyOutcome ApplyConfig(
    winrt::Windows::UI::Xaml::XamlRoot const& root, const Config& config);

// Puts every tracked taskbar back to its original appearance. Called when the
// settings become neutral and when the hook unloads.
void RestoreAll();

bool HasTrackedRoots();

// True when this root is already adjusted and every element the adjustment
// touched is still alive. Used by the periodic check that detects a taskbar
// rebuilt by a theme or accent change and re-applies the settings on top of the
// fresh tree without touching a healthy one.
bool IsRootTrackedAndAlive(
    winrt::Windows::UI::Xaml::XamlRoot const& root, const Config& config);

}  // namespace tuner::hook
