// Symbol resolution for taskbar.dll, following the approach of the Windhawk
// module this program is based on.
//
// The module resolves C++ symbols out of taskbar.pdb and uses them instead of
// guessing: the CTaskBand sub-object is recognised by comparing its vftable with
// the address of CTaskBand's `ITaskListWndSite' vftable, and the taskbar host is
// obtained by calling CTaskBand::GetTaskbarHost. Nothing is guessed, so no
// QueryInterface is ever called on a pointer that might not be an object.
//
// The one thing that has to be solved here is where the PDB comes from. Windows
// ships dbghelp.dll but not symsrv.dll, so dbghelp cannot fetch symbols from
// msdl.microsoft.com on its own. Instead the PDB is fetched by this module
// directly from the public symbol server, keyed by the exact signature stored in
// taskbar.dll, and handed to dbghelp as a plain local file. A PDB whose GUID/Age
// does not match the DLL can therefore never be picked up by accident, which is
// what made the "symbols loaded but every query fails" behaviour of the original
// attempt so confusing.
#pragma once

#include <windows.h>

#include <string>

namespace tuner::hook {

struct TaskbarSymbols {
    // `const CTaskBand::`vftable'{for `ITaskListWndSite'}`
    void* taskBandSiteVtable = nullptr;
    // `const CSecondaryTaskBand::`vftable'{for `ITaskListWndSite'}`
    void* secondaryTaskBandSiteVtable = nullptr;
    // std::shared_ptr<TaskbarHost> CTaskBand::GetTaskbarHost() const
    void* getTaskbarHost = nullptr;
    // std::shared_ptr<TaskbarHost> CSecondaryTaskBand::GetTaskbarHost() const
    void* getSecondaryTaskbarHost = nullptr;
    // int TaskbarHost::FrameHeight() const, used to read the element offset out
    // of its prologue instead of hard-coding 0x48.
    void* taskbarHostFrameHeight = nullptr;
    // std::_Ref_count_base::_Decref(), releases the shared_ptr returned above.
    void* refCountDecref = nullptr;

    // Offset of the taskbar FrameworkElement's IUnknown inside TaskbarHost.
    size_t elementOffset = 0x48;

    bool complete() const {
        return taskBandSiteVtable && secondaryTaskBandSiteVtable &&
               getTaskbarHost && getSecondaryTaskbarHost && refCountDecref;
    }
};

// Resolves the symbols once per process and caches the result. Safe to call from
// the hook worker thread; never call it from the taskbar UI thread because it
// may download a PDB.
bool EnsureTaskbarSymbols(std::wstring& error);

const TaskbarSymbols& GetTaskbarSymbols();

// Path the PDB was loaded from, for the log ("<pending>" before resolution).
const std::wstring& LoadedPdbPath();

}  // namespace tuner::hook
