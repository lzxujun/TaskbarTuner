// Locates the taskbar XamlRoot through taskbar.dll's own symbols.
//
// This follows the Windhawk module the program is based on: the symbol addresses
// of CTaskBand's `ITaskListWndSite' vftable and of CTaskBand::GetTaskbarHost are
// resolved once, the sub-object is identified by matching its vftable against
// that address, and the host is then obtained by calling the real function.
//
// An earlier version replaced this with a "scan the object for something that
// validates as XAML" heuristic, because symbols were believed to be unavailable
// without symsrv.dll. That heuristic is dangerous in a way the symbol-based path
// never is: it ends up calling QueryInterface on pointers that only look like
// objects, and a wrong AddRef on a live XAML object corrupts explorer's XAML
// state (observed as 0xC0000094 inside Windows.UI.Xaml.dll). Symbols are
// therefore back, with the PDB fetched by the module itself (see Symbols.h).
#pragma once

#include <windows.h>

#include <winrt/Windows.UI.Xaml.h>

namespace tuner::hook {

// Both functions must be called on the taskbar's UI thread: the XAML objects
// they return are apartment bound.
winrt::Windows::UI::Xaml::XamlRoot GetTaskbarXamlRoot(HWND hTaskbarWnd);
winrt::Windows::UI::Xaml::XamlRoot GetSecondaryTaskbarXamlRoot(
    HWND hSecondaryTaskbarWnd);

}  // namespace tuner::hook
