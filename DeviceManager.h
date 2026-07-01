// DeviceManager.h : USB-CDC COM port discovery and handle ownership.
#pragma once

#include <windows.h>
#include <optional>
#include <string>

// Enumerates present COM-port devices, matches VID/PID in the hardware ID,
// returns the port name (e.g. L"COM3") of the first match, or nullopt.
std::optional<std::wstring> ScanForDevice(USHORT vid, USHORT pid);

// Owns a single open COM port handle. Not thread-safe; use from the UI thread.
class DeviceManager {
public:
    ~DeviceManager();

    // Opens \\.\<port> for writing at Config::BAUD. Closes any prior handle.
    // Returns true on success.
    bool OpenPort(const std::wstring& port);
    void ClosePort();
    bool IsConnected() const { return handle_ != INVALID_HANDLE_VALUE; }
    HANDLE Handle() const { return handle_; }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};
