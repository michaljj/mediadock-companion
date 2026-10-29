// AudioMonitor.h : Core Audio default-render volume + device name with callbacks.
#pragma once

#include <windows.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <string>

// Watches the default render endpoint. The COM change callbacks arrive on
// internal MMDevAPI threads and do nothing but PostMessage to the UI window;
// all Core Audio object access (bind + read) happens on the UI thread via the
// Refresh*() handlers. That keeps every Core Audio object single-threaded, so
// no lock is needed and Stop() cannot deadlock against an in-flight callback.
// COM (CoInitializeEx) must already be initialized on the app.
class AudioMonitor {
public:
    void Start(HWND notifyHwnd, class StateStore* store);
    void Stop();

    // UI-thread handlers for WM_APP_AUDIO_VOLUME / WM_APP_AUDIO_DEVICE:
    // read Core Audio and update the StateStore.
    void RefreshVolume();
    void RefreshDevice();

    // Invoked by the internal COM callbacks on arbitrary threads. They only
    // PostMessage; they never touch the Core Audio objects below.
    void OnVolumeChanged();
    void OnDefaultDeviceChanged();

private:
    void BindDefaultEndpoint();        // (re)acquires device_ + endpointVol_
    int ReadVolume();                  // 0..100, -1 if unavailable
    bool ReadMute();                   // default endpoint mute state
    std::string ReadDeviceName();

    HWND hwnd_ = nullptr;
    StateStore* store_ = nullptr;

    // Touched only on the UI thread (Start/Stop/Refresh*).
    IMMDeviceEnumerator* enumerator_ = nullptr;
    IMMDevice* device_ = nullptr;
    IAudioEndpointVolume* endpointVol_ = nullptr;

    class VolumeCallback* volCb_ = nullptr;
    class NotificationClient* notifyCb_ = nullptr;
};
