// MediaMonitor.h : System Media Transport Controls (SMTC) track watcher.
#pragma once

#include <windows.h>
#include <thread>

// Watches the global media session for title/artist/playback changes and
// pushes them to the StateStore, posting WM_APP_STATE_CHANGED to the UI window.
// Runs all WinRT work on its own MTA thread so it never blocks the STA UI
// thread (SMTC async .get() on the UI STA thread would deadlock).
class MediaMonitor {
public:
    void Start(HWND notifyHwnd, class StateStore* store);
    void Stop();

private:
    void ThreadMain();

    std::thread thread_;
    HANDLE stopEvent_ = nullptr;
    HWND hwnd_ = nullptr;
    StateStore* store_ = nullptr;
};
