// MediaDockCompanion.cpp : WinMain-equivalent entry, hidden window, tray icon,
// message loop, and subsystem wiring.

#include "MediaDockCompanion.h"
#include "Config.h"
#include "Logger.h"

#include <windows.h>
#include <shellapi.h>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")

namespace {
    const wchar_t* kWindowClass = L"MediaDockCompanionWnd";
    NOTIFYICONDATAW g_nid = {};

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

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    Log("INFO", "shutdown");
    return 0;
}
