// Config.h : Project-wide constants (VID/PID, log path, baud, app messages).
#pragma once

#include <windows.h>

namespace Config {
    // TODO: real firmware VID/PID once known.
    constexpr USHORT VID = 0xCafe;
    constexpr USHORT PID = 0x4005;

    constexpr DWORD BAUD = 115200;

    // Subfolder under %LOCALAPPDATA% for the log file.
    inline constexpr wchar_t LOG_DIR_NAME[] = L"MediaDockCompanion";
    inline constexpr wchar_t LOG_FILE_NAME[] = L"log.txt";

    // Registry autostart value name under HKCU\...\Run.
    inline constexpr wchar_t AUTOSTART_VALUE[] = L"MediaDockCompanion";
}

// UI-thread signals posted from producer threads to the hidden window.
#define WM_APP_STATE_CHANGED  (WM_APP + 1)
#define WM_APP_DEVICE_ARRIVED (WM_APP + 2)
#define WM_APP_DEVICE_REMOVED (WM_APP + 3)

// Core Audio changes marshalled to the UI thread (COM work runs there, not
// in the callback, to avoid teardown deadlock / callback reentrancy).
#define WM_APP_AUDIO_VOLUME   (WM_APP + 5)
#define WM_APP_AUDIO_DEVICE   (WM_APP + 6)

// Tray icon callback + menu command ids.
#define WM_APP_TRAY           (WM_APP + 4)
#define IDM_AUTOSTART         1002
#define IDM_EXIT              1003
