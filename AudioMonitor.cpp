// AudioMonitor.cpp
#include "AudioMonitor.h"
#include "StateStore.h"
#include "Config.h"
#include "Logger.h"
#include "Util.h"

#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <cmath>
#include <new>

#pragma comment(lib, "ole32.lib")

namespace {
    template <class T> void SafeRelease(T*& p) {
        if (p) { p->Release(); p = nullptr; }
    }
}

// --- COM callback: volume changed ------------------------------------------
class VolumeCallback : public IAudioEndpointVolumeCallback {
public:
    explicit VolumeCallback(AudioMonitor* owner) : owner_(owner) {}

    STDMETHODIMP QueryInterface(REFIID iid, void** ppv) override {
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IAudioEndpointVolumeCallback)) {
            *ppv = static_cast<IAudioEndpointVolumeCallback*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&ref_); }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG r = InterlockedDecrement(&ref_);
        if (r == 0) delete this;
        return r;
    }
    STDMETHODIMP OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA) override {
        owner_->OnVolumeChanged();
        return S_OK;
    }

private:
    LONG ref_ = 1;
    AudioMonitor* owner_;
};

// --- COM callback: default device changed ----------------------------------
class NotificationClient : public IMMNotificationClient {
public:
    explicit NotificationClient(AudioMonitor* owner) : owner_(owner) {}

    STDMETHODIMP QueryInterface(REFIID iid, void** ppv) override {
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IMMNotificationClient)) {
            *ppv = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&ref_); }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG r = InterlockedDecrement(&ref_);
        if (r == 0) delete this;
        return r;
    }

    STDMETHODIMP OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        if (flow == eRender && role == eConsole) owner_->OnDefaultDeviceChanged();
        return S_OK;
    }
    STDMETHODIMP OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
    STDMETHODIMP OnDeviceAdded(LPCWSTR) override { return S_OK; }
    STDMETHODIMP OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    STDMETHODIMP OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

private:
    LONG ref_ = 1;
    AudioMonitor* owner_;
};

// --- AudioMonitor ----------------------------------------------------------
void AudioMonitor::Start(HWND notifyHwnd, StateStore* store) {
    hwnd_ = notifyHwnd;
    store_ = store;

    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
        CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
        reinterpret_cast<void**>(&enumerator_));
    if (FAILED(hr) || !enumerator_) {
        Log("ERROR", "MMDeviceEnumerator create failed");
        return;
    }

    volCb_ = new (std::nothrow) VolumeCallback(this);
    notifyCb_ = new (std::nothrow) NotificationClient(this);
    if (notifyCb_) enumerator_->RegisterEndpointNotificationCallback(notifyCb_);

    BindDefaultEndpoint();

    // Seed initial state and force a first send.
    store_->SetVolume(GetCurrentVolume());
    store_->SetDeviceName(GetCurrentDeviceName());
    PostChanged();
    Log("INFO", "AudioMonitor started");
}

void AudioMonitor::Stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (endpointVol_ && volCb_) endpointVol_->UnregisterControlChangeNotify(volCb_);
    SafeRelease(endpointVol_);
    SafeRelease(device_);
    if (enumerator_ && notifyCb_) enumerator_->UnregisterEndpointNotificationCallback(notifyCb_);
    SafeRelease(enumerator_);
    SafeRelease(volCb_);      // Release, not delete: COM-refcounted
    SafeRelease(notifyCb_);
}

void AudioMonitor::BindDefaultEndpoint() {
    std::lock_guard<std::mutex> lock(mutex_);
    // Unbind previous endpoint.
    if (endpointVol_ && volCb_) endpointVol_->UnregisterControlChangeNotify(volCb_);
    SafeRelease(endpointVol_);
    SafeRelease(device_);

    if (!enumerator_) return;
    HRESULT hr = enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, &device_);
    if (FAILED(hr) || !device_) {
        Log("WARN", "no default render endpoint");
        return;
    }
    hr = device_->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                           reinterpret_cast<void**>(&endpointVol_));
    if (SUCCEEDED(hr) && endpointVol_ && volCb_) {
        endpointVol_->RegisterControlChangeNotify(volCb_);
    }
}

void AudioMonitor::PostChanged() {
    if (hwnd_) PostMessageW(hwnd_, WM_APP_STATE_CHANGED, 0, 0);
}

int AudioMonitor::GetCurrentVolume() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!endpointVol_) return -1;
    float scalar = 0.0f;
    if (FAILED(endpointVol_->GetMasterVolumeLevelScalar(&scalar))) return -1;
    return static_cast<int>(std::lround(scalar * 100.0f));
}

std::string AudioMonitor::GetCurrentDeviceName() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!device_) return {};
    IPropertyStore* props = nullptr;
    if (FAILED(device_->OpenPropertyStore(STGM_READ, &props)) || !props) return {};

    PROPVARIANT v;
    PropVariantInit(&v);
    std::string name;
    if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR) {
        name = WideToUtf8(v.pwszVal);
    }
    PropVariantClear(&v);
    props->Release();
    return name;
}

void AudioMonitor::OnVolumeChanged() {
    if (!store_) return;
    store_->SetVolume(GetCurrentVolume());
    PostChanged();
}

void AudioMonitor::OnDefaultDeviceChanged() {
    if (!store_) return;
    BindDefaultEndpoint();
    store_->SetDeviceName(GetCurrentDeviceName());
    store_->SetVolume(GetCurrentVolume());
    PostChanged();
    Log("INFO", "default device changed");
}
