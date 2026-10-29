// MediaMonitor.cpp
#include "MediaMonitor.h"
#include "StateStore.h"
#include "Config.h"
#include "Logger.h"

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.Control.h>

#include <string>

using namespace winrt;
using namespace winrt::Windows::Media::Control;

namespace {
    using SessionManager = GlobalSystemMediaTransportControlsSessionManager;
    using Session = GlobalSystemMediaTransportControlsSession;
    using PlaybackStatus = GlobalSystemMediaTransportControlsSessionPlaybackStatus;

    const char* StatusToString(PlaybackStatus s) {
        switch (s) {
        case PlaybackStatus::Playing: return "Playing";
        case PlaybackStatus::Paused:  return "Paused";
        case PlaybackStatus::Stopped: return "Stopped";
        default:                      return "None";
        }
    }
}

void MediaMonitor::Start(HWND notifyHwnd, StateStore* store) {
    hwnd_ = notifyHwnd;
    store_ = store;
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stopEvent_) {
        Log("ERROR", "MediaMonitor CreateEvent failed");
        return;
    }
    thread_ = std::thread(&MediaMonitor::ThreadMain, this);
}

void MediaMonitor::Stop() {
    if (stopEvent_) SetEvent(stopEvent_);
    if (thread_.joinable()) thread_.join();
    if (stopEvent_) { CloseHandle(stopEvent_); stopEvent_ = nullptr; }
}

void MediaMonitor::ThreadMain() {
    init_apartment(apartment_type::multi_threaded);
    Log("INFO", "MediaMonitor thread started");

    try {
        SessionManager manager = SessionManager::RequestAsync().get();

        // Reads the current session and pushes track state to the store.
        auto readCurrent = [this, manager]() {
            try {
                Session session = manager.GetCurrentSession();
                if (!session) {
                    store_->SetTrack("", "", "None");
                } else {
                    auto props = session.TryGetMediaPropertiesAsync().get();
                    std::string title = to_string(props.Title());
                    std::string artist = to_string(props.Artist());
                    auto info = session.GetPlaybackInfo();
                    const char* status = StatusToString(info.PlaybackStatus());
                    store_->SetTrack(std::move(title), std::move(artist), status);
                }
            } catch (hresult_error const& e) {
                Log("WARN", "SMTC read failed: " + to_string(e.message()));
                return;
            }
            if (hwnd_) PostMessageW(hwnd_, WM_APP_STATE_CHANGED, 0, 0);
        };

        // Per-session subscriptions via auto-revoke, reset on each re-hook so
        // handlers on a closed/replaced session are released (no leak).
        Session::MediaPropertiesChanged_revoker propsRevoker;
        Session::PlaybackInfoChanged_revoker playbackRevoker;

        auto hookSession = [&, readCurrent](Session session) {
            propsRevoker.revoke();
            playbackRevoker.revoke();
            if (session) {
                propsRevoker = session.MediaPropertiesChanged(auto_revoke,
                    [readCurrent](Session const&, auto const&) { readCurrent(); });
                playbackRevoker = session.PlaybackInfoChanged(auto_revoke,
                    [readCurrent](Session const&, auto const&) { readCurrent(); });
            }
        };

        auto mgrRevoker = manager.CurrentSessionChanged(auto_revoke,
            [&, readCurrent, hookSession](SessionManager const& m, auto const&) {
                hookSession(m.GetCurrentSession());
                readCurrent();
            });

        hookSession(manager.GetCurrentSession());
        readCurrent();

        // Keep the MTA alive until Stop() signals; callbacks run on RPC threads.
        WaitForSingleObject(stopEvent_, INFINITE);
    } catch (hresult_error const& e) {
        Log("ERROR", "MediaMonitor setup failed: " + to_string(e.message()));
    }

    uninit_apartment();
    Log("INFO", "MediaMonitor thread stopped");
}
