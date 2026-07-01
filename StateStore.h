// StateStore.h : Mutex-guarded snapshot of everything sent to the dock.
#pragma once

#include <mutex>
#include <string>

struct AppState {
    int volume = -1;                 // 0..100, -1 = unknown
    std::string deviceName;          // default render device friendly name
    std::string title;
    std::string artist;
    std::string playback = "None";   // "Playing" / "Paused" / "Stopped" / "None"
};

// Thread-safe. Any setter that changes a value marks the store dirty.
class StateStore {
public:
    void SetVolume(int v);
    void SetDeviceName(std::string name);
    void SetTrack(std::string title, std::string artist, std::string playback);

    AppState Snapshot();
    // Returns true (and clears the flag) if state changed since the last call.
    bool TakeDirty();

private:
    std::mutex mutex_;
    AppState state_;
    bool dirty_ = false;
};
