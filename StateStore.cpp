// StateStore.cpp
#include "StateStore.h"

void StateStore::SetVolume(int v, bool muted) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_.volume != v || state_.muted != muted) {
        state_.volume = v;
        state_.muted = muted;
        dirty_ = true;
    }
}

void StateStore::SetDeviceName(std::string name) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_.deviceName != name) {
        state_.deviceName = std::move(name);
        dirty_ = true;
    }
}

void StateStore::SetTrack(std::string title, std::string artist, std::string playback) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_.title != title || state_.artist != artist || state_.playback != playback) {
        state_.title = std::move(title);
        state_.artist = std::move(artist);
        state_.playback = std::move(playback);
        dirty_ = true;
    }
}

AppState StateStore::Snapshot() {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

bool StateStore::TakeDirty() {
    std::lock_guard<std::mutex> lock(mutex_);
    bool was = dirty_;
    dirty_ = false;
    return was;
}
