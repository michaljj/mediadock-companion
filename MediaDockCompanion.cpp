// MediaDockCompanion.cpp : WinMain-equivalent entry, hidden window, tray icon,
// message loop, and subsystem wiring.

#include "MediaDockCompanion.h"
#include "Config.h"
#include "Logger.h"
#include "DeviceManager.h"
#include "StateStore.h"
#include "AudioMonitor.h"
#include "MediaMonitor.h"
#include "Serializer.h"
#include "Startup.h"

#include <windows.h>
#include <shellapi.h>
#include <dbt.h>
#include <objbase.h>

#include <cstdio>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")

namespace {
    const wchar_t* kWindowClass = L"MediaDockCompanionWnd";
    UINT g_taskbarCreated = 0;   // "TaskbarCreated" broadcast; re-add tray icon
    NOTIFYICONDATAW g_nid = {};
    DeviceManager g_device;
    HDEVNOTIFY g_devNotify = nullptr;
    StateStore g_state;
    AudioMonitor g_audio;
    MediaMonitor g_media;

    // Debug mode: ignore the dock, print state as JSON to a console instead.
    bool g_debug = false;

    // Attach to the launching console, or allocate one if started detached,
    // then route stdout/stderr there (WIN32 subsystem has no console by default).
    void SetupDebugConsole() {
        if (!AttachConsole(ATTACH_PARENT_PROCESS)) AllocConsole();
        FILE* f = nullptr;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
    }

    void PrintState() {
        std::string json = SerializeState(g_state.Snapshot());
        std::fputs(json.c_str(), stdout);
        std::fputc('\n', stdout);
        std::fflush(stdout);
    }

    // GUID_DEVINTERFACE_COMPORT (ntddser.h) — declared inline to avoid initguid.
    const GUID kComPortIface = {
        0x86E0D1E0, 0x8089, 0x11D0,
        {0x9C, 0xE4, 0x08, 0x00, 0x3E, 0x30, 0x1F, 0x73}};

    void RegisterComPortNotifications(HWND hwnd) {
        DEV_BROADCAST_DEVICEINTERFACE filter = {};
        filter.dbcc_size = sizeof(filter);
        filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
        filter.dbcc_classguid = kComPortIface;
        g_devNotify = RegisterDeviceNotificationW(
            hwnd, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);
        if (!g_devNotify) Log("ERROR", "RegisterDeviceNotification failed");
    }

    constexpr UINT_PTR kReconnectTimer = 1;
    constexpr UINT kReconnectDelayMs = 2000;

    // Writes the full current state to the dock, ignoring the dirty flag.
    // On write failure the port is closed; returns false so the caller can
    // schedule a reconnect (the dock may still be physically present).
    bool SendNow() {
        if (!g_device.IsConnected()) return false;
        if (WriteToDevice(g_device.Handle(), SerializeState(g_state.Snapshot())))
            return true;
        g_device.ClosePort();
        return false;
    }

    // Writes only when state actually changed since the last send.
    // In debug mode, print to the console instead of touching the dock.
    void SendIfDirty(HWND hwnd) {
        if (!g_state.TakeDirty()) return;
        if (g_debug) { PrintState(); return; }
        // A failed write closes the port; retry via the reconnect timer so a
        // transient error does not silently drop the dock until replug.
        if (g_device.IsConnected() && !SendNow())
            SetTimer(hwnd, kReconnectTimer, kReconnectDelayMs, nullptr);
    }

    enum class ConnectResult { Connected, NotPresent, OpenFailed };

    // Scan + open once; push full state on success.
    ConnectResult TryConnect() {
        if (g_device.IsConnected()) return ConnectResult::Connected;
        auto port = ScanForDevice(Config::VID, Config::PID);
        if (!port) return ConnectResult::NotPresent;       // wait for arrival notif
        if (!g_device.OpenPort(*port)) return ConnectResult::OpenFailed;
        SendNow();  // a failed first write closes the port again
        return g_device.IsConnected() ? ConnectResult::Connected
                                      : ConnectResult::OpenFailed;
    }

    // Retry only when the dock is present but not yet open. When it is absent we
    // rely on the WM_DEVICECHANGE arrival notification instead of polling
    // forever (which would spin + spam the log at startup with no dock).
    void ScheduleReconnectIfNeeded(HWND hwnd, ConnectResult r) {
        if (r == ConnectResult::OpenFailed)
            SetTimer(hwnd, kReconnectTimer, kReconnectDelayMs, nullptr);
        else
            KillTimer(hwnd, kReconnectTimer);
    }

    void OnDeviceArrived(HWND hwnd) {
        ScheduleReconnectIfNeeded(hwnd, TryConnect());
    }

    void AddTrayIcon(HWND hwnd) {
        g_nid.cbSize = sizeof(g_nid);
        g_nid.hWnd = hwnd;
        g_nid.uID = 1;
        g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        g_nid.uCallbackMessage = WM_APP_TRAY;
        g_nid.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
        lstrcpynW(g_nid.szTip, L"Media Dock Companion", ARRAYSIZE(g_nid.szTip));
        if (!Shell_NotifyIconW(NIM_ADD, &g_nid)) Log("WARN", "tray icon add failed");
    }

    void RemoveTrayIcon() {
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
    }

    void ShowContextMenu(HWND hwnd) {
        POINT pt;
        GetCursorPos(&pt);
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING | (IsAutostartEnabled() ? MF_CHECKED : 0),
                    IDM_AUTOSTART, L"Start with Windows");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Exit");

        // Required so the menu dismisses correctly for tray windows.
        SetForegroundWindow(hwnd);
        TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
        DestroyMenu(menu);
    }

    LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
        // Explorer restarted -> the tray icon was dropped; re-add it.
        if (msg == g_taskbarCreated && g_taskbarCreated != 0) {
            AddTrayIcon(hwnd);
            return 0;
        }
        switch (msg) {
        case WM_APP_TRAY:
            if (LOWORD(lparam) == WM_RBUTTONUP || LOWORD(lparam) == WM_CONTEXTMENU) {
                ShowContextMenu(hwnd);
            }
            return 0;

        case WM_DEVICECHANGE:
            if (wparam == DBT_DEVICEARRIVAL) {
                PostMessageW(hwnd, WM_APP_DEVICE_ARRIVED, 0, 0);
            } else if (wparam == DBT_DEVICEREMOVECOMPLETE) {
                PostMessageW(hwnd, WM_APP_DEVICE_REMOVED, 0, 0);
            }
            return TRUE;

        case WM_APP_DEVICE_ARRIVED:
            OnDeviceArrived(hwnd);
            return 0;

        case WM_APP_DEVICE_REMOVED:
            // Removal fires for ANY COM port (the notification filter is the
            // whole port interface class). Only drop the link if OUR device is
            // actually gone, so unplugging an unrelated serial device does not
            // kill the dock connection.
            if (g_device.IsConnected() && !ScanForDevice(Config::VID, Config::PID)) {
                KillTimer(hwnd, kReconnectTimer);
                g_device.ClosePort();
            }
            return 0;

        case WM_TIMER:
            if (wparam == kReconnectTimer)
                ScheduleReconnectIfNeeded(hwnd, TryConnect());
            return 0;

        case WM_APP_STATE_CHANGED:
            SendIfDirty(hwnd);
            return 0;

        case WM_APP_AUDIO_VOLUME:
            g_audio.RefreshVolume();   // Core Audio read on the UI thread
            SendIfDirty(hwnd);
            return 0;

        case WM_APP_AUDIO_DEVICE:
            g_audio.RefreshDevice();   // rebind + read on the UI thread
            SendIfDirty(hwnd);
            return 0;

        case WM_COMMAND:
            switch (LOWORD(wparam)) {
            case IDM_AUTOSTART:
                SetAutostart(!IsAutostartEnabled());
                return 0;
            case IDM_EXIT:
                Log("INFO", "Exit clicked");
                DestroyWindow(hwnd);
                return 0;
            }
            return 0;

        case WM_DESTROY:
            RemoveTrayIcon();
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
}

int main() {
    Log("INFO", "startup");

    // Single instance: autostart + a manual launch would otherwise fight over
    // the tray icon and the (exclusively-opened) COM port. Bail if a copy
    // already holds the named mutex. Handle is released on process exit.
    HANDLE singleton = CreateMutexW(nullptr, TRUE, L"Local\\MediaDockCompanion-singleton");
    if (singleton && GetLastError() == ERROR_ALREADY_EXISTS) {
        Log("INFO", "another instance already running, exiting");
        return 0;
    }

    // Parse args: --debug prints state to a console and ignores the dock.
    if (int argc = 0; LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        for (int i = 1; i < argc; ++i) {
            if (wcscmp(argv[i], L"--debug") == 0) g_debug = true;
        }
        LocalFree(argv);
    }
    if (g_debug) {
        SetupDebugConsole();
        std::fputs("MediaDockCompanion debug mode - printing state, dock ignored.\n",
                   stdout);
        std::fflush(stdout);
    }

    // UI thread is STA (window + tray + Core Audio). WinRT SMTC runs on its
    // own MTA worker thread (see MediaMonitor) to avoid apartment conflict.
    HRESULT hrCom = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hrCom)) {
        Log("ERROR", "CoInitializeEx failed");
        return 1;
    }

    HINSTANCE hInst = GetModuleHandleW(nullptr);

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = kWindowClass;
    if (!RegisterClassW(&wc)) {
        Log("ERROR", "RegisterClass failed");
        return 1;
    }

    // Broadcast sent by the shell when Explorer (re)starts; used to re-add the
    // tray icon so it survives an explorer.exe restart.
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    // Hidden top-level window (never shown). NOT message-only: message-only
    // windows do not receive broadcast messages, and the TaskbarCreated
    // broadcast is what lets us re-add the tray icon after an Explorer restart.
    // WS_EX_TOOLWINDOW keeps it off the taskbar / Alt-Tab.
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, kWindowClass, L"MediaDockCompanion",
                                WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr, hInst, nullptr);
    if (!hwnd) {
        Log("ERROR", "CreateWindowEx failed");
        return 1;
    }

    AddTrayIcon(hwnd);
    // Seed audio/media state before the first connect so the initial frame
    // pushed to the dock carries real values, not the -1/empty defaults.
    g_audio.Start(hwnd, &g_state);
    g_media.Start(hwnd, &g_state);
    if (!g_debug) {
        RegisterComPortNotifications(hwnd);
        OnDeviceArrived(hwnd); // device may already be connected at launch
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g_media.Stop();
    g_audio.Stop();
    if (g_devNotify) UnregisterDeviceNotification(g_devNotify);
    g_device.ClosePort();
    CoUninitialize();

    Log("INFO", "shutdown");
    return 0;
}
