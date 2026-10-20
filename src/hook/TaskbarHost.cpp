#include "TaskbarHost.h"

#include "Symbols.h"

#include "../shared/Log.h"

#include <cstdint>
#include <cstring>

#include <unknwn.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Xaml.h>

using namespace winrt::Windows::UI::Xaml;

namespace tuner::hook {
namespace {

// CTaskBand implements several interfaces, so the pointer stored in the window's
// user data is not necessarily the sub-object that carries the
// `ITaskListWndSite' vftable. The sub-object lives within the first few slots of
// the object and is identified by that vftable, which is exactly how the
// Windhawk module does it.
constexpr size_t kMaxSiteSlots = 20;

using GetTaskbarHostFn = void*(WINAPI*)(void* self, void** result);
using DecrefFn = void(WINAPI*)(void* self);

bool IsReadable(const void* pointer, size_t bytes) {
    if (!pointer) {
        return false;
    }

    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(pointer, &info, sizeof(info)) != sizeof(info)) {
        return false;
    }

    if (info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
        return false;
    }

    const uintptr_t start = reinterpret_cast<uintptr_t>(pointer);
    const uintptr_t end = start + bytes;
    const uintptr_t regionEnd =
        reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
    return end > start && end <= regionEnd;
}

// QueryInterface is still wrapped: the pointer comes from a symbol-matched
// sub-object and should be a real IUnknown, but a fault must not take explorer
// down. This wrapper holds no C++ object so it can use SEH.
HRESULT SafeQueryInterface(IUnknown* unknown, const GUID& iid, void** result) {
    __try {
        return unknown->QueryInterface(iid, result);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return E_FAIL;
    }
}

// Returns the sub-object whose vftable matches, i.e. the ITaskListWndSite
// interface pointer that GetTaskbarHost expects as `this`.
void* FindSiteObject(void* taskBand, void* siteVtable) {
    if (!taskBand || !siteVtable) {
        return nullptr;
    }

    for (size_t slot = 0; slot < kMaxSiteSlots; slot++) {
        auto* candidate = static_cast<void**>(taskBand) + slot;
        if (!IsReadable(candidate, sizeof(void*))) {
            return nullptr;
        }
        if (*candidate == siteVtable) {
            return candidate;
        }
    }

    TLOG(L"no ITaskListWndSite sub-object within %zu slots", kMaxSiteSlots);
    return nullptr;
}

// shared_ptr<TaskbarHost> is returned through a hidden out parameter: [0] is the
// TaskbarHost and [1] its control block, which has to be released with
// std::_Ref_count_base::_Decref.
XamlRoot XamlRootFromHost(void* site, void* getTaskbarHost,
                          const TaskbarSymbols& symbols) {
    if (!site || !getTaskbarHost || !symbols.refCountDecref) {
        return nullptr;
    }

    void* shared[2]{};
    reinterpret_cast<GetTaskbarHostFn>(getTaskbarHost)(site, shared);

    XamlRoot result = nullptr;
    if (shared[0]) {
        auto* elementUnknown = reinterpret_cast<IUnknown**>(
            reinterpret_cast<BYTE*>(shared[0]) + symbols.elementOffset);
        if (IsReadable(elementUnknown, sizeof(void*)) && *elementUnknown) {
            GUID iid{};
            const auto frameworkElementGuid = winrt::guid_of<FrameworkElement>();
            memcpy(&iid, &frameworkElementGuid, sizeof(GUID));

            void* abi = nullptr;
            if (SUCCEEDED(SafeQueryInterface(*elementUnknown, iid, &abi)) && abi) {
                FrameworkElement element{abi, winrt::take_ownership_from_abi};
                try {
                    result = element.XamlRoot();
                } catch (...) {
                    TLOG(L"XamlRoot() threw for %p", abi);
                }
            } else {
                TLOG(L"TaskbarHost+0x%zX is not a FrameworkElement",
                     symbols.elementOffset);
            }
        }
    } else {
        TLOG(L"GetTaskbarHost returned an empty pointer");
    }

    if (shared[1]) {
        reinterpret_cast<DecrefFn>(symbols.refCountDecref)(shared[1]);
    }

    return result;
}

XamlRoot Resolve(HWND taskSwWnd, void* siteVtable, void* getTaskbarHost) {
    if (!taskSwWnd) {
        return nullptr;
    }

    void* taskBand = reinterpret_cast<void*>(GetWindowLongPtrW(taskSwWnd, 0));
    if (!taskBand) {
        TLOG(L"taskband window %p has no CTaskBand", taskSwWnd);
        return nullptr;
    }

    void* site = FindSiteObject(taskBand, siteVtable);
    if (!site) {
        return nullptr;
    }

    return XamlRootFromHost(site, getTaskbarHost, GetTaskbarSymbols());
}

}  // namespace

XamlRoot GetTaskbarXamlRoot(HWND hTaskbarWnd) {
    if (!hTaskbarWnd) {
        return nullptr;
    }

    std::wstring error;
    if (!EnsureTaskbarSymbols(error)) {
        TLOG(L"taskbar symbols unavailable: %s", error.c_str());
        return nullptr;
    }

    HWND hTaskSwWnd =
        reinterpret_cast<HWND>(GetPropW(hTaskbarWnd, L"TaskbandHWND"));
    if (!hTaskSwWnd) {
        TLOG(L"taskband window not found for %p", hTaskbarWnd);
        return nullptr;
    }

    const TaskbarSymbols& symbols = GetTaskbarSymbols();
    return Resolve(hTaskSwWnd, symbols.taskBandSiteVtable, symbols.getTaskbarHost);
}

XamlRoot GetSecondaryTaskbarXamlRoot(HWND hSecondaryTaskbarWnd) {
    if (!hSecondaryTaskbarWnd) {
        return nullptr;
    }

    std::wstring error;
    if (!EnsureTaskbarSymbols(error)) {
        TLOG(L"taskbar symbols unavailable: %s", error.c_str());
        return nullptr;
    }

    HWND hTaskSwWnd =
        FindWindowExW(hSecondaryTaskbarWnd, nullptr, L"WorkerW", nullptr);
    if (!hTaskSwWnd) {
        TLOG(L"secondary taskband window not found for %p", hSecondaryTaskbarWnd);
        return nullptr;
    }

    const TaskbarSymbols& symbols = GetTaskbarSymbols();
    return Resolve(hTaskSwWnd, symbols.secondaryTaskBandSiteVtable,
                   symbols.getSecondaryTaskbarHost);
}

}  // namespace tuner::hook
