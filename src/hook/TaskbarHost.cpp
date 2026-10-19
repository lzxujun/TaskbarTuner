#include "TaskbarHost.h"

#include "../shared/Log.h"

#include <algorithm>
#include <cstring>
#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include <unknwn.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Media.h>

using namespace winrt::Windows::UI::Xaml;

namespace tuner::hook {
namespace {

// TaskbarHost is a small object; the element pointer sits at a fixed offset
// that was 0x48 on every build the original author tested. The list is tried
// in order so that the common case costs one read, and the remaining entries
// cover a build where the class grew.
constexpr size_t kElementOffsets[] = {
    0x48, 0x40, 0x50, 0x38, 0x58, 0x30, 0x60, 0x28,
    0x68, 0x70, 0x78, 0x20, 0x80, 0x88, 0x90, 0x98,
};

constexpr size_t kScanBytes = 0x400;

void* g_cachedTaskBand = nullptr;
bool g_hasCache = false;

ProbeStats g_stats;

bool IsReadable(const void* pointer, size_t bytes) {
    if (!pointer) {
        return false;
    }

    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(pointer, &info, sizeof(info)) != sizeof(info)) {
        return false;
    }

    if (info.State != MEM_COMMIT) {
        return false;
    }

    if (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) {
        return false;
    }

    const uintptr_t start = reinterpret_cast<uintptr_t>(pointer);
    const uintptr_t end = start + bytes;
    const uintptr_t regionEnd =
        reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;

    return end > start && end <= regionEnd;
}

// A vtable lives in the read-only data of a mapped image, so the page type is
// enough to tell a real object from a random heap value.
bool InImage(uintptr_t address) {
    if (!address) {
        return false;
    }

    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(reinterpret_cast<const void*>(address), &info,
                     sizeof(info)) != sizeof(info)) {
        return false;
    }

    return info.Type == MEM_IMAGE && info.State == MEM_COMMIT;
}

// Checks that a candidate really looks like the start of an object: the first
// few entries of a vtable are code pointers in the same image. This rejects
// almost every wrong pointer before a single virtual call is made.
bool LooksLikeObject(void* object) {
    if (!IsReadable(object, sizeof(void*) * 4)) {
        return false;
    }

    auto* vtable = *reinterpret_cast<uintptr_t**>(object);
    if (!InImage(reinterpret_cast<uintptr_t>(vtable))) {
        return false;
    }

    if (!IsReadable(vtable, sizeof(void*) * 4)) {
        return false;
    }

    for (int i = 0; i < 4; i++) {
        if (!InImage(vtable[i])) {
            return false;
        }
    }

    return true;
}

// QueryInterface is called on a pointer that came out of a heuristic scan, so
// a fault has to be contained. This wrapper deliberately holds no C++ object:
// a function that needs unwinding cannot use structured exception handling.
HRESULT SafeQueryInterface(IUnknown* unknown, const GUID& iid, void** result) {
    __try {
        return unknown->QueryInterface(iid, result);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return E_FAIL;
    }
}

bool HasDescendantNamed(FrameworkElement element, PCWSTR name, int depth,
                        int& budget) {
    if (depth > 9 || budget <= 0) {
        return false;
    }

    budget--;

    try {
        if (element.Name() == name) {
            return true;
        }

        const int count = Media::VisualTreeHelper::GetChildrenCount(element);
        for (int i = 0; i < count; i++) {
            auto child = Media::VisualTreeHelper::GetChild(element, i)
                             .try_as<FrameworkElement>();
            if (child && HasDescendantNamed(child, name, depth + 1, budget)) {
                return true;
            }
        }
    } catch (...) {
        // The tree can be torn down between two calls.
    }

    return false;
}

// Turns a raw IUnknown into a XamlRoot, but only after the element proved that
// it belongs to the taskbar and not to some unrelated WinRT object that
// happened to sit in the same heap block.
XamlRoot ValidateElement(void* raw) {
    // The probe walks thousands of candidates; the first few failures are
    // reported so that an unsupported build can be diagnosed from the log.
    static int reported = 0;

    if (!raw || !LooksLikeObject(raw)) {
        return nullptr;
    }

    GUID iid{};
    const auto frameworkElementGuid = winrt::guid_of<FrameworkElement>();
    memcpy(&iid, &frameworkElementGuid, sizeof(GUID));

    void* abi = nullptr;
    if (FAILED(SafeQueryInterface(reinterpret_cast<IUnknown*>(raw), iid, &abi)) ||
        !abi) {
        return nullptr;
    }

    FrameworkElement element{ abi, winrt::take_ownership_from_abi };

    try {
        auto root = element.XamlRoot();
        if (!root) {
            if (reported++ < 6) {
                TLOG(L"%p is an element but has no XamlRoot", raw);
            }
            return nullptr;
        }

        auto content = root.Content();
        if (!content) {
            if (reported++ < 6) {
                TLOG(L"%p has an empty XamlRoot", raw);
            }
            return nullptr;
        }

        auto contentElement = content.try_as<FrameworkElement>();
        if (!contentElement) {
            if (reported++ < 6) {
                TLOG(L"%p: XamlRoot content is not a FrameworkElement", raw);
            }
            return nullptr;
        }

        int budget = 400;
        if (!HasDescendantNamed(contentElement, L"BackgroundFill", 0, budget)) {
            if (reported++ < 6) {
                TLOG(L"%p belongs to another XAML island (no BackgroundFill)",
                     raw);
            }
            return nullptr;
        }

        return root;
    } catch (...) {
        return nullptr;
    }
}

// A host candidate is a heap block, never a vtable or a piece of an image.
bool IsHeapBlock(void* address) {
    if (!address || !IsReadable(address, sizeof(void*))) {
        return false;
    }

    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(address, &info, sizeof(info)) != sizeof(info)) {
        return false;
    }

    return info.Type == MEM_PRIVATE && info.State == MEM_COMMIT;
}

// True when the pointer points into one of the XAML implementations. A XAML
// object always comes from there, so this rejects almost every word in a heap
// block without ever dereferencing it.
//
// The answer is learned per module on first sight instead of being resolved
// once up front: the taskbar's XAML is delay-loaded, and a probe that runs
// before it appears would otherwise record an empty module list and then
// reject every candidate for the rest of the explorer process. The module is
// identified by its file name, which survives the renames between builds.
bool IsXamlModuleBase(HMODULE base) {
    struct Known {
        HMODULE base;
        bool xaml;
    };
    static Known known[32]{};
    static int knownCount = 0;

    for (int i = 0; i < knownCount; i++) {
        if (known[i].base == base) {
            return known[i].xaml;
        }
    }

    wchar_t path[MAX_PATH]{};
    bool xaml = false;
    if (GetModuleFileNameW(base, path, MAX_PATH)) {
        const std::wstring_view name(path);
        std::wstring lowered(name);
        std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                       [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
        xaml = lowered.find(L"xaml") != std::wstring::npos;
    }

    if (knownCount < static_cast<int>(std::size(known))) {
        known[knownCount++] = {base, xaml};
    }

    return xaml;
}

bool InXamlModule(void* address) {
    if (!address) {
        return false;
    }

    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(address, &info, sizeof(info)) != sizeof(info)) {
        return false;
    }

    if (info.Type != MEM_IMAGE) {
        return false;
    }

    return IsXamlModuleBase(static_cast<HMODULE>(info.AllocationBase));
}

// Reads one pointer out of a block that may be anything at all.
void* ReadPtr(void* base, size_t offset) {
    auto* address = reinterpret_cast<BYTE*>(base) + offset;
    if (!IsReadable(address, sizeof(void*))) {
        return nullptr;
    }

    return *reinterpret_cast<void**>(address);
}

// True when the word at host+offset is a XAML object, which is the only kind of
// value worth spending a QueryInterface on.
bool IsXamlObjectAt(void* host, size_t offset) {
    void* raw = ReadPtr(host, offset);
    if (!raw || !IsReadable(raw, sizeof(void*))) {
        return false;
    }

    return InXamlModule(*reinterpret_cast<void**>(raw));
}

// Where the taskbar host was found: a pointer inside the taskband object, an
// optional pointer inside that object, and the offset of the element inside the
// host. Cached after the first success because the layout of a running explorer
// never changes.
struct HostLocation {
    size_t slot = 0;
    size_t inner = 0;
    size_t element = 0;
    bool valid = false;
};

HostLocation g_location;

XamlRoot ResolveLocation(void* taskBand, const HostLocation& location) {
    void* host = ReadPtr(taskBand, location.slot);
    if (location.inner) {
        host = ReadPtr(host, location.inner);
    }

    if (!host) {
        return nullptr;
    }

    return ValidateElement(ReadPtr(host, location.element));
}

// Tries every known element offset inside one host candidate.
XamlRoot TryHost(void* taskBand, HostLocation location) {
    void* host = ReadPtr(taskBand, location.slot);
    if (location.inner) {
        host = ReadPtr(host, location.inner);
    }

    if (!host) {
        return nullptr;
    }

    for (size_t offset : kElementOffsets) {
        if (!IsXamlObjectAt(host, offset)) {
            continue;
        }

        g_stats.attempts++;

        if (auto root = ValidateElement(ReadPtr(host, offset))) {
            location.element = offset;
            location.valid = true;
            g_location = location;
            g_cachedTaskBand = taskBand;
            g_hasCache = true;

            g_stats.hostSlot = location.slot;
            g_stats.elementOffset = offset;

            TLOG(L"taskbar host found: slot 0x%llX, inner 0x%llX, element 0x%llX "
                 L"(%d attempts)",
                 static_cast<unsigned long long>(location.slot),
                 static_cast<unsigned long long>(location.inner),
                 static_cast<unsigned long long>(offset), g_stats.attempts);
            return root;
        }
    }

    return nullptr;
}

XamlRoot ProbeTaskBand(void* taskBand) {
    g_stats = {};

    if (!taskBand || !IsReadable(taskBand, sizeof(void*))) {
        return nullptr;
    }

    // The layout does not change while explorer lives, so a successful probe is
    // remembered and the next apply costs a couple of pointer reads.
    if (g_hasCache && g_cachedTaskBand == taskBand && g_location.valid) {
        if (auto root = ResolveLocation(taskBand, g_location)) {
            g_stats.usedCache = true;
            g_stats.hostSlot = g_location.slot;
            g_stats.elementOffset = g_location.element;
            return root;
        }

        TLOG(L"cached taskbar host layout no longer resolves, rescanning");
        g_hasCache = false;
    }

    // TaskbarHost is a plain C++ object with no vtable of its own (FrameHeight
    // is a non-virtual member), so it cannot be recognised by its shape. What
    // it is recognised by is its content: at the element offset it holds a XAML
    // object, and that object must be an element of the taskbar's own XAML
    // island. That validation cannot accept a wrong pointer.
    //
    // Depth zero is the direct case (the host pointer sits in the taskband
    // object). Depth one covers builds where it hangs off a task list or site
    // object instead.
    for (size_t slot = 0; slot < kScanBytes; slot += sizeof(void*)) {
        HostLocation location;
        location.slot = slot;
        if (TryHost(taskBand, location)) {
            return ResolveLocation(taskBand, g_location);
        }
    }

    for (size_t slot = 0; slot < kScanBytes; slot += sizeof(void*)) {
        void* container = ReadPtr(taskBand, slot);
        if (!IsHeapBlock(container)) {
            continue;
        }

        for (size_t inner = 0; inner < 0x100; inner += sizeof(void*)) {
            HostLocation location;
            location.slot = slot;
            location.inner = inner;
            if (TryHost(taskBand, location)) {
                return ResolveLocation(taskBand, g_location);
            }
        }
    }

    // Only the first few empty scans are reported: the probe runs again on
    // every verification pass, and a build where one candidate keeps failing
    // would otherwise fill the log with the same line every few seconds.
    static int emptyScans = 0;
    if (emptyScans++ < 3) {
        TLOG(L"taskbar host not found after %d attempts", g_stats.attempts);
    }
    return nullptr;
}

// The taskband window is normally reachable through a window property. Not
// every build sets it, so every child of the tray window is used as a fallback:
// whichever child holds an object that validates as a taskbar XAML root wins.
// Guessing a class name would be fragile - the class has been renamed more than
// once - while the validation cannot accept a wrong object.
void CollectChildObjects(HWND parent, std::vector<void*>& objects) {
    EnumChildWindows(
        parent,
        [](HWND hWnd, LPARAM lParam) -> BOOL {
            void* object =
                reinterpret_cast<void*>(GetWindowLongPtrW(hWnd, 0));
            if (object) {
                reinterpret_cast<std::vector<void*>*>(lParam)->push_back(object);
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&objects));
}

XamlRoot ProbeChildren(HWND parent) {
    std::vector<void*> objects;
    CollectChildObjects(parent, objects);

    // Every candidate costs a full slot scan, so the list is capped: the
    // taskband is among the first children, and the default taskbar has a
    // handful of them. Without a cap a tray window with many children would
    // turn a failed probe into a long stall on the taskbar's UI thread.
    constexpr size_t kMaxCandidates = 8;
    if (objects.size() > kMaxCandidates) {
        objects.resize(kMaxCandidates);
    }

    for (void* object : objects) {
        if (auto root = ProbeTaskBand(object)) {
            return root;
        }
    }

    return nullptr;
}

}  // namespace

XamlRoot GetTaskbarXamlRoot(HWND hTaskbarWnd) {
    if (!hTaskbarWnd) {
        return nullptr;
    }

    HWND hTaskSwWnd =
        reinterpret_cast<HWND>(GetPropW(hTaskbarWnd, L"TaskbandHWND"));
    if (hTaskSwWnd) {
        void* taskBand =
            reinterpret_cast<void*>(GetWindowLongPtrW(hTaskSwWnd, 0));
        if (taskBand) {
            if (auto root = ProbeTaskBand(taskBand)) {
                return root;
            }
        }
    }

    auto root = ProbeChildren(hTaskbarWnd);
    if (!root) {
        TLOG(L"no taskbar host found for %p", hTaskbarWnd);
    }
    return root;
}

XamlRoot GetSecondaryTaskbarXamlRoot(HWND hSecondaryTaskbarWnd) {
    if (!hSecondaryTaskbarWnd) {
        return nullptr;
    }

    // A secondary taskbar hosts its XAML island in a WorkerW child; on builds
    // that do not, the scan below finds it anyway.
    if (HWND hWorkerW =
            FindWindowExW(hSecondaryTaskbarWnd, nullptr, L"WorkerW", nullptr)) {
        void* taskBand = reinterpret_cast<void*>(GetWindowLongPtrW(hWorkerW, 0));
        if (taskBand) {
            if (auto root = ProbeTaskBand(taskBand)) {
                return root;
            }
        }
    }

    return ProbeChildren(hSecondaryTaskbarWnd);
}

const ProbeStats& LastProbeStats() {
    return g_stats;
}

void ResetProbeCache() {
    g_hasCache = false;
    g_cachedTaskBand = nullptr;
    g_location.valid = false;
}

}  // namespace tuner::hook
