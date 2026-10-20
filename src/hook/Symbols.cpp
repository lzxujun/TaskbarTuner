#include "Symbols.h"

#include "../shared/Log.h"

#include <dbghelp.h>
#include <urlmon.h>

#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "urlmon.lib")

namespace tuner::hook {
namespace {

// The taskbar symbols this program needs, written the way the PDB stores them.
// The mangled spelling is tried first because it is exact; the readable
// signature is only used by the enumeration fallback below, so a build that
// changes an unrelated part of the declaration still resolves.
struct SymbolRequest {
    const wchar_t* mangled;
    const char* signatureFragment;  // matched against the undecorated name
    void* TaskbarSymbols::* slot;
    bool secondaryOnly;             // fragment must also contain "Secondary"
};

const SymbolRequest kRequests[] = {
    {L"??_7CTaskBand@@6BITaskListWndSite@@@",
     "CTaskBand::`vftable'{for `ITaskListWndSite'}", &TaskbarSymbols::taskBandSiteVtable,
     false},
    {L"??_7CSecondaryTaskBand@@6BITaskListWndSite@@@",
     "CSecondaryTaskBand::`vftable'{for `ITaskListWndSite'}",
     &TaskbarSymbols::secondaryTaskBandSiteVtable, true},
    {L"?GetTaskbarHost@CTaskBand@@UEBA?AV?$shared_ptr@VTaskbarHost@@@std@@XZ",
     "CTaskBand::GetTaskbarHost(", &TaskbarSymbols::getTaskbarHost, false},
    {L"?GetTaskbarHost@CSecondaryTaskBand@@UEBA?AV?$shared_ptr@VTaskbarHost@@@std@@XZ",
     "CSecondaryTaskBand::GetTaskbarHost(", &TaskbarSymbols::getSecondaryTaskbarHost, true},
    {L"?FrameHeight@TaskbarHost@@QEBAHXZ",
     "TaskbarHost::FrameHeight(", &TaskbarSymbols::taskbarHostFrameHeight, false},
    {L"?_Decref@_Ref_count_base@std@@QEAAXXZ",
     "_Ref_count_base::_Decref(", &TaskbarSymbols::refCountDecref, false},
};

TaskbarSymbols g_symbols;
bool g_resolved = false;
// Resolution can fail for a transient reason (no network for the PDB), but a
// permanent one (unsupported build) must not be retried on every apply pass.
int g_attempts = 0;
constexpr int kMaxAttempts = 3;
std::wstring g_lastError;
std::wstring g_pdbPath;
HMODULE g_taskbarModule = nullptr;
DWORD64 g_taskbarBase = 0;

std::wstring LocalAppData() {
    wchar_t buffer[MAX_PATH * 2]{};
    const DWORD length =
        GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, ARRAYSIZE(buffer));
    if (length == 0 || length >= ARRAYSIZE(buffer)) {
        wchar_t temp[MAX_PATH * 2]{};
        GetTempPathW(ARRAYSIZE(temp), temp);
        return temp;
    }
    return buffer;
}

std::wstring SymbolCacheDir() {
    std::wstring dir = LocalAppData() + L"\\TaskbarTuner\\symbols";
    CreateDirectoryW((LocalAppData() + L"\\TaskbarTuner").c_str(), nullptr);
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

// Reads the CodeView record of a PE image: the PDB file name and the
// "<GUID><Age>" key the symbol server indexes it by.
bool ReadPdbSignature(const std::wstring& imagePath, std::wstring& pdbName,
                      std::wstring& key) {
    HANDLE file = CreateFileW(imagePath.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    const BYTE* base = mapping
                           ? static_cast<const BYTE*>(
                                 MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0))
                           : nullptr;
    if (!base) {
        CloseHandle(file);
        if (mapping) {
            CloseHandle(mapping);
        }
        return false;
    }

    bool ok = false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic == IMAGE_DOS_SIGNATURE) {
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        if (nt->Signature == IMAGE_NT_SIGNATURE) {
            const IMAGE_DATA_DIRECTORY& debugDir =
                nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
            const auto* sections = IMAGE_FIRST_SECTION(nt);

            auto rvaToOffset = [&](DWORD rva) -> const BYTE* {
                for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
                    const DWORD size = sections[i].SizeOfRawData
                                           ? sections[i].SizeOfRawData
                                           : sections[i].Misc.VirtualSize;
                    if (rva >= sections[i].VirtualAddress &&
                        rva < sections[i].VirtualAddress + size) {
                        return base + sections[i].PointerToRawData +
                               (rva - sections[i].VirtualAddress);
                    }
                }
                return nullptr;
            };

            if (debugDir.VirtualAddress && debugDir.Size) {
                const auto* entries = reinterpret_cast<const IMAGE_DEBUG_DIRECTORY*>(
                    rvaToOffset(debugDir.VirtualAddress));
                const size_t count = debugDir.Size / sizeof(IMAGE_DEBUG_DIRECTORY);
                for (size_t i = 0; entries && i < count; i++) {
                    if (entries[i].Type != IMAGE_DEBUG_TYPE_CODEVIEW) {
                        continue;
                    }
                    // AddressOfRawData is an RVA; the file offset is
                    // PointerToRawData, and the record is read from the mapped
                    // image, so the offset is what is needed here.
                    const BYTE* data = entries[i].PointerToRawData
                                           ? base + entries[i].PointerToRawData
                                           : rvaToOffset(entries[i].AddressOfRawData);
                    if (!data || memcmp(data, "RSDS", 4) != 0) {
                        continue;
                    }

                    // RSDS: 4 bytes magic, 16 bytes GUID, 4 bytes Age, name.
                    const BYTE* guid = data + 4;
                    const DWORD age = *reinterpret_cast<const DWORD*>(data + 20);
                    wchar_t buffer[64]{};
                    swprintf_s(buffer,
                               L"%08X%04X%04X%02X%02X%02X%02X%02X%02X%02X%02X%X",
                               *reinterpret_cast<const DWORD*>(guid),
                               *reinterpret_cast<const WORD*>(guid + 4),
                               *reinterpret_cast<const WORD*>(guid + 6), guid[8],
                               guid[9], guid[10], guid[11], guid[12], guid[13],
                               guid[14], guid[15], age);
                    key = buffer;
                    // The record stores the PDB name as ANSI.
                    const char* name = reinterpret_cast<const char*>(data + 24);
                    wchar_t wide[260]{};
                    MultiByteToWideChar(CP_ACP, 0, name, -1, wide,
                                        ARRAYSIZE(wide));
                    pdbName = wide;
                    ok = !pdbName.empty() && !key.empty();
                    break;
                }
            }
        }
    }

    UnmapViewOfFile(base);
    CloseHandle(mapping);
    CloseHandle(file);
    return ok;
}

// Downloads the PDB straight from the public symbol server. dbghelp cannot do
// this by itself because symsrv.dll is not part of Windows.
bool FetchPdb(const std::wstring& cacheDir, const std::wstring& pdbName,
              const std::wstring& key, std::wstring& error) {
    const std::wstring target = cacheDir + L"\\" + pdbName;
    if (GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES) {
        g_pdbPath = target;
        return true;
    }

    std::wstring url = L"https://msdl.microsoft.com/download/symbols/" + pdbName +
                       L"/" + key + L"/" + pdbName;
    const std::wstring partial = target + L".part";
    DeleteFileW(partial.c_str());

    TLOG(L"downloading %s", url.c_str());
    const HRESULT hr = URLDownloadToFileW(nullptr, url.c_str(), partial.c_str(), 0,
                                          nullptr);
    if (FAILED(hr)) {
        DeleteFileW(partial.c_str());
        wchar_t buffer[96]{};
        swprintf_s(buffer, L"PDB 下载失败（0x%08X）", static_cast<unsigned>(hr));
        error = buffer;
        return false;
    }

    if (!MoveFileExW(partial.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(partial.c_str());
        error = L"无法写入符号缓存：" + target;
        return false;
    }

    g_pdbPath = target;
    TLOG(L"pdb cached at %s", target.c_str());
    return true;
}

// The element offset is read from TaskbarHost::FrameHeight's prologue:
//
//   48:83EC 28 | sub rsp,28
//   48:83C1 xx | add rcx,xx     <- xx is the offset of the element
//
// Hard-coding 0x48 works on the builds seen so far, but following the module's
// approach means the offset keeps working when the class grows.
size_t DeriveElementOffset(void* frameHeight) {
    constexpr size_t kFallback = 0x48;
    if (!frameHeight) {
        return kFallback;
    }

    const auto* code = static_cast<const BYTE*>(frameHeight);
    if (code[0] == 0x48 && code[1] == 0x83 && code[2] == 0xEC && code[4] == 0x48 &&
        code[5] == 0x83 && code[6] == 0xC1 && code[7] <= 0x7F) {
        return code[7];
    }

    TLOG(L"unsupported TaskbarHost::FrameHeight prologue, using 0x%zX", kFallback);
    return kFallback;
}

struct EnumContext {
    const SymbolRequest* request;
    void* address;
};

BOOL CALLBACK EnumProc(PSYMBOL_INFOW info, ULONG, PVOID userContext) {
    auto* context = static_cast<EnumContext*>(userContext);
    if (context->address) {
        return FALSE;
    }

    // UnDecorateSymbolName only takes ANSI; mangled names are ASCII by
    // construction, so the conversion is lossless.
    char mangled[MAX_SYM_NAME]{};
    if (!WideCharToMultiByte(CP_ACP, 0, info->Name, -1, mangled,
                             ARRAYSIZE(mangled), nullptr, nullptr)) {
        return TRUE;
    }

    char undecorated[2048]{};
    if (!UnDecorateSymbolName(mangled, undecorated, ARRAYSIZE(undecorated),
                              UNDNAME_COMPLETE)) {
        return TRUE;
    }

    const std::string_view name(undecorated);
    if (name.find(context->request->signatureFragment) == std::string_view::npos) {
        return TRUE;
    }
    // "CTaskBand::GetTaskbarHost" is a substring of the secondary band's
    // spelling, so the secondary symbols are matched explicitly instead of by
    // substring alone.
    const bool secondary = name.find("CSecondaryTaskBand") != std::string_view::npos;
    if (secondary != context->request->secondaryOnly) {
        return TRUE;
    }

    context->address = reinterpret_cast<void*>(info->Address);
    return FALSE;
}

// SizeOfImage of the mapped module, or 0 when the headers cannot be read.
DWORD ImageSize(HMODULE module) {
    const auto* base = reinterpret_cast<const BYTE*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return 0;
    }

    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return 0;
    }
    return nt->OptionalHeader.SizeOfImage;
}

// One full resolution attempt: locate taskbar.dll, stage its PDB, load symbols.
bool ResolveSymbolsOnce(std::wstring& error) {
    g_taskbarModule = GetModuleHandleW(L"taskbar.dll");
    if (!g_taskbarModule) {
        g_taskbarModule =
            LoadLibraryExW(L"taskbar.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    }
    if (!g_taskbarModule) {
        error = L"无法加载 taskbar.dll";
        return false;
    }

    wchar_t modulePath[MAX_PATH * 2]{};
    if (!GetModuleFileNameW(g_taskbarModule, modulePath, ARRAYSIZE(modulePath))) {
        error = L"无法取得 taskbar.dll 路径";
        return false;
    }

    g_taskbarBase = reinterpret_cast<DWORD64>(g_taskbarModule);

    const std::wstring cacheDir = SymbolCacheDir();
    std::wstring pdbName;
    std::wstring key;
    if (!ReadPdbSignature(modulePath, pdbName, key)) {
        error = L"taskbar.dll 中没有调试信息";
        return false;
    }

    TLOG(L"taskbar.dll pdb %s (%s)", pdbName.c_str(), key.c_str());

    if (!FetchPdb(cacheDir, pdbName, key, error)) {
        return false;
    }

    // dbghelp searches the directory it is given for "<pdbname>" and validates
    // the signature inside the file, so a cached PDB from another build is
    // rejected rather than silently accepted.
    if (!SymInitializeW(GetCurrentProcess(), cacheDir.c_str(), FALSE)) {
        wchar_t buffer[96]{};
        swprintf_s(buffer, L"SymInitialize 失败（%lu）", GetLastError());
        error = buffer;
        return false;
    }
    SymSetOptions(SYMOPT_FAIL_CRITICAL_ERRORS);

    const DWORD64 loaded = SymLoadModuleExW(
        GetCurrentProcess(), nullptr, modulePath, nullptr, g_taskbarBase, 0,
        nullptr, 0);
    if (!loaded) {
        wchar_t buffer[96]{};
        swprintf_s(buffer, L"SymLoadModuleEx 失败（%lu）", GetLastError());
        error = buffer;
        return false;
    }

    const DWORD imageSizeBytes = ImageSize(g_taskbarModule);
    if (!imageSizeBytes) {
        error = L"无法读取 taskbar.dll 的映像大小";
        return false;
    }
    const DWORD64 imageEnd = g_taskbarBase + imageSizeBytes;

    size_t missing = 0;
    for (const auto& request : kRequests) {
        void* address = nullptr;

        // Exact mangled name first.
        union {
            SYMBOL_INFOW info;
            BYTE storage[sizeof(SYMBOL_INFOW) + MAX_SYM_NAME * sizeof(WCHAR)];
        } symbol{};
        symbol.info.SizeOfStruct = sizeof(SYMBOL_INFOW);
        symbol.info.MaxNameLen = MAX_SYM_NAME;
        if (SymFromNameW(GetCurrentProcess(), request.mangled, &symbol.info)) {
            address = reinterpret_cast<void*>(symbol.info.Address);
        }

        // Fallback: walk the symbol table and compare readable names.
        if (!address) {
            EnumContext context{&request, nullptr};
            SymEnumSymbolsW(GetCurrentProcess(), g_taskbarBase, nullptr, EnumProc,
                            &context);
            address = context.address;
            if (address) {
                TLOG(L"resolved %s by signature", request.mangled);
            }
        }

        if (!address) {
            missing++;
            TLOG(L"symbol not found: %s", request.mangled);
            continue;
        }

        // The PDB is not trusted: the cache directory is an ordinary user
        // directory, so a planted file could declare any address at all. Every
        // resolved address has to land inside taskbar.dll itself, which is the
        // same check Windhawk's symbol loader performs.
        const DWORD64 addressValue = reinterpret_cast<DWORD64>(address);
        if (addressValue < g_taskbarBase || addressValue >= imageEnd) {
            missing++;
            TLOG(L"symbol %s resolved to 0x%llX, outside taskbar.dll "
                 L"(0x%llX..0x%llX)",
                 request.mangled, addressValue, g_taskbarBase, imageEnd);
            continue;
        }

        g_symbols.*request.slot = address;
    }

    if (missing) {
        wchar_t buffer[96]{};
        swprintf_s(buffer, L"缺少 %zu 个任务栏符号", missing);
        error = buffer;
        return false;
    }

    g_symbols.elementOffset =
        DeriveElementOffset(g_symbols.taskbarHostFrameHeight);
    g_resolved = true;

    TLOG(L"taskbar symbols ready: band site %p, host %p, element offset 0x%zX",
         g_symbols.taskBandSiteVtable, g_symbols.getTaskbarHost,
         g_symbols.elementOffset);
    return true;
}

}  // namespace

bool EnsureTaskbarSymbols(std::wstring& error) {
    if (g_resolved) {
        return true;
    }

    if (g_attempts >= kMaxAttempts) {
        error = g_lastError;
        return false;
    }

    g_attempts++;
    if (ResolveSymbolsOnce(error)) {
        g_resolved = true;
        return true;
    }

    g_lastError = error;
    if (g_attempts >= kMaxAttempts) {
        TLOG(L"giving up on symbols after %d attempts: %s", g_attempts,
             error.c_str());
    }
    return false;
}

const TaskbarSymbols& GetTaskbarSymbols() { return g_symbols; }

const std::wstring& LoadedPdbPath() { return g_pdbPath; }

}  // namespace tuner::hook
