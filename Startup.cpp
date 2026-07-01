// Startup.cpp
#include "Startup.h"
#include "Config.h"
#include "Logger.h"

#include <windows.h>
#include <string>

namespace {
    constexpr wchar_t kRunKey[] =
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

    std::wstring ExePath() {
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
        return path;
    }
}

bool IsAutostartEnabled() {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return false;
    LONG r = RegQueryValueExW(key, Config::AUTOSTART_VALUE, nullptr, nullptr,
                              nullptr, nullptr);
    RegCloseKey(key);
    return r == ERROR_SUCCESS;
}

void SetAutostart(bool enable) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
        Log("ERROR", "open Run key failed");
        return;
    }
    if (enable) {
        std::wstring path = ExePath();
        RegSetValueExW(key, Config::AUTOSTART_VALUE, 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(path.c_str()),
                       static_cast<DWORD>((path.size() + 1) * sizeof(wchar_t)));
        Log("INFO", "autostart enabled");
    } else {
        RegDeleteValueW(key, Config::AUTOSTART_VALUE);
        Log("INFO", "autostart disabled");
    }
    RegCloseKey(key);
}
