// Locates the taskbar XamlRoot without a debugger, a PDB or a symbol server.
//
// The original TaskbarTuner resolved taskbar.dll symbols with dbghelp and
// called CTaskBand::GetTaskbarHost. That approach failed on this machine for a
// reason that is easy to miss: Windows does not ship symsrv.dll, so dbghelp can
// only use PDBs that already exist on disk. It found a stale Taskbar.pdb from
// the Windhawk cache, marked the module as "symbols loaded", and then failed
// every single query because the GUID did not match.
//
// This module does not need any of that. The taskband window's user data is
// the CTaskBand object; somewhere inside it lives the TaskbarHost, and inside
// the TaskbarHost there is an IUnknown for the taskbar's root FrameworkElement.
// Instead of knowing the exact layout, the object is walked slot by slot and
// each candidate is validated against the live XAML tree. Only a pointer that
// resolves to a real FrameworkElement whose XamlRoot has a BackgroundFill can
// pass, so the false positive rate is effectively zero.
#pragma once

#include <windows.h>

#include <winrt/Windows.UI.Xaml.h>

namespace tuner::hook {

// Both functions must be called on the taskbar's UI thread: the XAML objects
// they return are apartment bound.
winrt::Windows::UI::Xaml::XamlRoot GetTaskbarXamlRoot(HWND hTaskbarWnd);
winrt::Windows::UI::Xaml::XamlRoot GetSecondaryTaskbarXamlRoot(
    HWND hSecondaryTaskbarWnd);

// Diagnostic counters for the log; the first probe is the only expensive one.
struct ProbeStats {
    int attempts = 0;
    bool usedCache = false;
    size_t hostSlot = 0;
    size_t elementOffset = 0;
};

const ProbeStats& LastProbeStats();

// Drops the cached layout, e.g. after explorer restarted or the taskbar was
// rebuilt.
void ResetProbeCache();

}  // namespace tuner::hook
