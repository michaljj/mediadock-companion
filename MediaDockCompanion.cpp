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

#include <windows.h>
#include <shellapi.h>
#include <dbt.h>
#include <objbase.h>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")

namespace {
    const wchar_t* kWindowClass = L"MediaDockCompanionWnd";
    NOTIFYICONDATAW g_nid = {};
    DeviceManager g_device;
    HDEVNOTIFY g_devNotify = nullptr;
    StateStore g_state;
    AudioMonitor g_audio;
    MediaMonitor g_media;

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

    // Writes the full current state to the dock, ignoring the dirty flag.
    void SendNow() {
        if (!g_device.IsConnected()) return;
        if (!WriteToDevice(g_device.Handle(), SerializeState(g_state.Snapshot()))) {
            g_device.ClosePort();
        }
    }

    // Writes only when state actually changed since the last send.
    void SendIfDirty() {
        if (g_device.IsConnected() && g_state.TakeDirty()) SendNow();
    }

    // Scan + open when a matching device arrives; push full state immediately.
    void OnDeviceArrived() {
        if (auto port = ScanForDevice(Config::VID, Config::PID)) {
            if (g_device.OpenPort(*port)) SendNow();
        }
    }

    void AddTrayIcon(HWND hwnd) {
        g_nid.cbSize = sizeof(g_nid);
        g_nid.hWnd = hwnd;
        g_nid.uID = 1;
        g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        g_nid.uCallbackMessage = WM_APP_TRAY;
        g_nid.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
        lstrcpynW(g_nid.szTip, L"Media Dock Companion", ARRAYSIZE(g_nid.szTip));
        Shell_NotifyIconW(NIM_ADD, &g_nid);
    }

    void RemoveTrayIcon() {
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
    }

    void ShowContextMenu(HWND hwnd) {
        POINT pt;
        GetCursorPos(&pt);
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, IDM_SHOW, L"Show");
        AppendMenuW(menu, MF_STRING, IDM_AUTOSTART, L"Start with Windows");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Exit");

        // Required so the menu dismisses correctly for tray windows.
        SetForegroundWindow(hwnd);
        TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
        DestroyMenu(menu);
    }

    LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
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
            OnDeviceArrived();
            return 0;

        case WM_APP_DEVICE_REMOVED:
            g_device.ClosePort();
            return 0;

        case WM_APP_STATE_CHANGED:
            SendIfDirty();
            return 0;

        case WM_COMMAND:
            switch (LOWORD(wparam)) {
            case IDM_SHOW:
                Log("INFO", "Show clicked");
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
    RegisterClassW(&wc);

    // Hidden message window (never shown).
    HWND hwnd = CreateWindowExW(0, kWindowClass, L"MediaDockCompanion",
                                0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, hInst, nullptr);
    if (!hwnd) {
        Log("ERROR", "CreateWindowEx failed");
        return 1;
    }

    AddTrayIcon(hwnd);
    RegisterComPortNotifications(hwnd);
    OnDeviceArrived(); // device may already be connected at launch
    g_audio.Start(hwnd, &g_state);
    g_media.Start(hwnd, &g_state);

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
