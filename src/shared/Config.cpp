#include "Config.h"

#include <cstdio>
#include <string>
#include <vector>

namespace tuner {
namespace {

int ReadInt(HKEY key, const wchar_t* name, int fallback) {
    DWORD type = 0;
    DWORD value = 0;
    DWORD size = sizeof(value);
    if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(&value),
                         &size) == ERROR_SUCCESS &&
        (type == REG_DWORD || type == REG_DWORD_BIG_ENDIAN)) {
        return static_cast<int>(value);
    }
    return fallback;
}

int Clamp(int value, int lo, int hi) {
    return value < lo ? lo : (value > hi ? hi : value);
}

void WriteInt(HKEY key, const wchar_t* name, int value) {
    DWORD data = static_cast<DWORD>(value);
    RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&data),
                   sizeof(data));
}

// The startup entry is stored as a command line, so the executable path has to
// be quoted: the installation path contains spaces on most machines.
std::wstring ModulePath() {
    wchar_t buffer[MAX_PATH * 2]{};
    DWORD length = GetModuleFileNameW(nullptr, buffer, ARRAYSIZE(buffer));
    return std::wstring(buffer, length);
}

// The startup entry is the only place that records "start with Windows", so it
// is read back instead of being duplicated in the settings key. That way
// disabling it from Task Manager is picked up on the next start.
bool IsAutoStartEnabled() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &key) !=
        ERROR_SUCCESS) {
        return false;
    }

    wchar_t value[1024]{};
    DWORD size = sizeof(value);
    DWORD type = 0;
    const bool present =
        RegQueryValueExW(key, kRunValue, nullptr, &type,
                         reinterpret_cast<BYTE*>(value), &size) == ERROR_SUCCESS &&
        type == REG_SZ;
    RegCloseKey(key);

    return present;
}

}  // namespace

Config LoadConfig() {
    Config config;

    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegKeyRoot, 0, KEY_READ, &key) !=
        ERROR_SUCCESS) {
        return config;
    }

    config.opacity = Clamp(ReadInt(key, L"Opacity", 100), 0, 100);
    config.iconOpacity = Clamp(ReadInt(key, L"IconOpacity", 100), 0, 100);
    config.topLine = ReadInt(key, L"TopLine", 1) != 0;
    // The drag gripper switch of earlier builds is gone from the window, so the
    // stored value is not read any more: a leftover HideGrip = 1 would hide the
    // handle with no way to bring it back. SaveConfig writes it back as 0.
    config.hideGrip = false;
    config.showAllTray = ReadInt(key, L"ShowAllTray", 0) != 0;
    config.enabled = ReadInt(key, L"Enabled", 1) != 0;

    // Brightness is stored on the 0..100 scale the window shows, 100 meaning
    // "unchanged". Every older layout - including the first program, which
    // wrote no version at all - stored it as 0..-100 with 0 meaning
    // "unchanged": that configuration has Brightness = 0 while its window
    // showed 100%. Those values are converted once, here, so that an existing
    // configuration keeps producing exactly the same look.
    const int version = ReadInt(key, L"CfgVersion", 0);
    int brightness = ReadInt(key, L"Brightness", 100);
    if (version != kConfigVersion) {
        brightness = 100 + Clamp(brightness, -100, 0);
    }
    config.brightness = Clamp(brightness, 0, 100);

    RegCloseKey(key);

    config.autoStart = IsAutoStartEnabled();
    return config;
}

bool SaveConfig(const Config& config) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegKeyRoot, 0, nullptr, 0, KEY_WRITE,
                        nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return false;
    }

    WriteInt(key, L"Opacity", config.opacity);
    WriteInt(key, L"Brightness", config.brightness);
    WriteInt(key, L"IconOpacity", config.iconOpacity);
    WriteInt(key, L"TopLine", config.topLine ? 1 : 0);
    WriteInt(key, L"HideGrip", config.hideGrip ? 1 : 0);
    WriteInt(key, L"ShowAllTray", config.showAllTray ? 1 : 0);
    WriteInt(key, L"Enabled", config.enabled ? 1 : 0);
    WriteInt(key, L"CfgVersion", kConfigVersion);

    RegCloseKey(key);
    return true;
}

HWND FindHookWindow() {
    return FindWindowW(kHookWindowClass, kHookWindowTitle);
}

void RequestHookSuspend() {
    if (HWND hookWnd = FindHookWindow()) {
        PostMessageW(hookWnd, kMsgSuspend, 0, 0);
    }
}

void RequestHookShutdown() {
    if (HWND hookWnd = FindHookWindow()) {
        // The window is gone by the time the message is handled, so the answer
        // is not waited for: the DLL unloads itself after restoring.
        PostMessageW(hookWnd, kMsgShutdown, 0, 0);
        Sleep(120);
    }
}

bool PushConfig(const Config& config) {
    SaveConfig(config);

    HWND hookWnd = FindHookWindow();
    if (!hookWnd) {
        return false;
    }

    PostMessageW(hookWnd, kMsgSettingsChanged, 0, 0);
    return true;
}

void SetAutoStart(bool enabled) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_WRITE,
                        nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return;
    }

    if (enabled) {
        std::wstring command = L"\"" + ModulePath() + L"\" --tray";
        RegSetValueExW(key, kRunValue, 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(command.c_str()),
                       static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, kRunValue);
    }

    RegCloseKey(key);
}

namespace {

constexpr wchar_t kNotifyIconSettings[] = L"Control Panel\\NotifyIconSettings";
constexpr wchar_t kBackupKey[] = L"Software\\TaskbarTuner\\TrayIconsBackup";

struct BackupEntry {
    std::wstring keyName;
    DWORD original = 0;
};

std::vector<BackupEntry> LoadBackup() {
    std::vector<BackupEntry> entries;

    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kBackupKey, 0, KEY_READ, &key) !=
        ERROR_SUCCESS) {
        return entries;
    }

    for (DWORD index = 0;; index++) {
        wchar_t subName[64]{};
        DWORD subLength = ARRAYSIZE(subName);
        if (RegEnumKeyExW(key, index, subName, &subLength, nullptr, nullptr,
                          nullptr, nullptr) != ERROR_SUCCESS) {
            break;
        }

        HKEY sub = nullptr;
        if (RegOpenKeyExW(key, subName, 0, KEY_READ, &sub) != ERROR_SUCCESS) {
            continue;
        }

        BackupEntry entry;
        wchar_t nameBuffer[256]{};
        DWORD nameSize = sizeof(nameBuffer);
        DWORD type = 0;
        if (RegQueryValueExW(sub, L"KeyName", nullptr, &type,
                             reinterpret_cast<BYTE*>(nameBuffer),
                             &nameSize) == ERROR_SUCCESS) {
            entry.keyName = nameBuffer;
            entry.original = static_cast<DWORD>(ReadInt(sub, L"Original", 0));
            entries.push_back(entry);
        }

        RegCloseKey(sub);
    }

    RegCloseKey(key);
    return entries;
}

void ClearBackup() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kBackupKey, 0, KEY_ALL_ACCESS, &key) !=
        ERROR_SUCCESS) {
        return;
    }

    // Delete children first; the key itself is left in place, which is
    // harmless and avoids racing with a concurrent reader.
    for (;;) {
        wchar_t subName[64]{};
        DWORD subLength = ARRAYSIZE(subName);
        if (RegEnumKeyExW(key, 0, subName, &subLength, nullptr, nullptr, nullptr,
                          nullptr) != ERROR_SUCCESS) {
            break;
        }
        RegDeleteTreeW(key, subName);
    }

    RegCloseKey(key);
}

bool IsPromoted(DWORD& value, const std::wstring& keyName) {
    HKEY iconKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      (std::wstring(kNotifyIconSettings) + L"\\" + keyName).c_str(),
                      0, KEY_READ, &iconKey) != ERROR_SUCCESS) {
        return false;
    }

    int raw = ReadInt(iconKey, L"IsPromoted", 0);
    value = static_cast<DWORD>(raw);
    RegCloseKey(iconKey);
    return true;
}

void SetPromoted(const std::wstring& keyName, DWORD value) {
    HKEY iconKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      (std::wstring(kNotifyIconSettings) + L"\\" + keyName).c_str(),
                      0, KEY_SET_VALUE, &iconKey) != ERROR_SUCCESS) {
        return;
    }

    WriteInt(iconKey, L"IsPromoted", static_cast<int>(value));
    RegCloseKey(iconKey);
}

}  // namespace

bool SetShowAllTrayIcons(bool enabled, const Config& config) {
    (void)config;

    HKEY settings = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kNotifyIconSettings, 0, KEY_READ,
                      &settings) != ERROR_SUCCESS) {
        return false;
    }

    if (!enabled) {
        // Put every remembered value back, then forget the backup so that a
        // later enable captures the values that are current at that point.
        for (const auto& entry : LoadBackup()) {
            SetPromoted(entry.keyName, entry.original);
        }
        ClearBackup();
        RegCloseKey(settings);
        return true;
    }

    // A backup that already exists must not be replaced: the values it holds
    // are the originals, and re-capturing them now would store "promoted" as
    // the original. The flags themselves are re-applied, because the shell
    // resets them when a new session starts.
    if (auto backup = LoadBackup(); !backup.empty()) {
        for (const auto& entry : backup) {
            SetPromoted(entry.keyName, 1);
        }
        RegCloseKey(settings);
        return true;
    }

    HKEY backup = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kBackupKey, 0, nullptr, 0,
                        KEY_WRITE, nullptr, &backup, nullptr) != ERROR_SUCCESS) {
        RegCloseKey(settings);
        return false;
    }

    DWORD slot = 0;
    for (DWORD index = 0;; index++) {
        wchar_t subName[256]{};
        DWORD subLength = ARRAYSIZE(subName);
        if (RegEnumKeyExW(settings, index, subName, &subLength, nullptr, nullptr,
                          nullptr, nullptr) != ERROR_SUCCESS) {
            break;
        }

        DWORD original = 0;
        if (!IsPromoted(original, subName)) {
            continue;
        }

        wchar_t slotName[16]{};
        swprintf_s(slotName, L"%u", slot++);

        HKEY slotKey = nullptr;
        if (RegCreateKeyExW(backup, slotName, 0, nullptr, 0, KEY_WRITE, nullptr,
                            &slotKey, nullptr) != ERROR_SUCCESS) {
            continue;
        }

        RegSetValueExW(slotKey, L"KeyName", 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(subName),
                       static_cast<DWORD>((wcslen(subName) + 1) * sizeof(wchar_t)));
        WriteInt(slotKey, L"Original", static_cast<int>(original));
        RegCloseKey(slotKey);

        SetPromoted(subName, 1);
    }

    RegCloseKey(backup);
    RegCloseKey(settings);
    return true;
}

}  // namespace tuner
