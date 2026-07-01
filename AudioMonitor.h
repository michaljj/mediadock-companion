// AudioMonitor.h : Core Audio default-render volume + device name with callbacks.
#pragma once

#include <windows.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <mutex>
#include <string>

// Watches the default render endpoint. On a volume change or default-device
// change it updates the StateStore and posts WM_APP_STATE_CHANGED to the UI
// window. COM (CoInitializeEx) must already be initialized on the app.
class AudioMonitor {
public:
    void Start(HWND notifyHwnd, class StateStore* store);
    void Stop();

    int GetCurrentVolume();            // 0..100, -1 if unavailable
    std::string GetCurrentDeviceName();

    // Invoked by the internal COM callbacks (public so the callback classes
    // can reach them; not part of the intended external surface).
    void OnVolumeChanged();
    void OnDefaultDeviceChanged();

private:
    void BindDefaultEndpoint();        // (re)acquires device_ + endpointVol_
    void UnbindEndpoint();
    void PostChanged();

    HWND hwnd_ = nullptr;
    StateStore* store_ = nullptr;

    std::mutex mutex_;                 // guards the COM pointers below
    IMMDeviceEnumerator* enumerator_ = nullptr;
    IMMDevice* device_ = nullptr;
    IAudioEndpointVolume* endpointVol_ = nullptr;

    class VolumeCallback* volCb_ = nullptr;
    class NotificationClient* notifyCb_ = nullptr;
};
