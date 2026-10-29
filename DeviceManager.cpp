// DeviceManager.cpp
#include "DeviceManager.h"
#include "Config.h"
#include "Logger.h"

#include <windows.h>
#include <setupapi.h>
#include <devguid.h>
#include <cwchar>
#include <string>

#pragma comment(lib, "setupapi.lib")

namespace {
    // Builds "VID_XXXX" / "PID_XXXX" uppercase tokens for substring matching.
    std::wstring HexToken(const wchar_t* prefix, USHORT value) {
        wchar_t buf[16];
        swprintf(buf, ARRAYSIZE(buf), L"%s_%04X", prefix, value);
        return buf;
    }

    // Reads the "PortName" (e.g. "COM3") from the device's hardware reg key.
    std::optional<std::wstring> ReadPortName(HDEVINFO set, SP_DEVINFO_DATA& data) {
        HKEY key = SetupDiOpenDevRegKey(set, &data, DICS_FLAG_GLOBAL, 0,
                                        DIREG_DEV, KEY_READ);
        if (key == INVALID_HANDLE_VALUE) return std::nullopt;

        wchar_t name[16] = {};
        DWORD size = sizeof(name);
        DWORD type = 0;
        LONG r = RegQueryValueExW(key, L"PortName", nullptr, &type,
                                  reinterpret_cast<LPBYTE>(name), &size);
        RegCloseKey(key);
        if (r != ERROR_SUCCESS || type != REG_SZ) return std::nullopt;
        // Registry strings are not guaranteed null-terminated; bound the read
        // to the buffer so a value that fills it cannot over-read.
        return std::wstring(name, wcsnlen(name, ARRAYSIZE(name)));
    }
}

std::optional<std::wstring> ScanForDevice(USHORT vid, USHORT pid) {
    HDEVINFO set = SetupDiGetClassDevsW(&GUID_DEVCLASS_PORTS, nullptr, nullptr,
                                        DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) {
        Log("ERROR", "SetupDiGetClassDevs failed");
        return std::nullopt;
    }

    const std::wstring vidTok = HexToken(L"VID", vid);
    const std::wstring pidTok = HexToken(L"PID", pid);

    std::optional<std::wstring> result;
    SP_DEVINFO_DATA data = {};
    data.cbSize = sizeof(data);

    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &data) && !result; ++i) {
        wchar_t hwid[512] = {};
        if (!SetupDiGetDeviceRegistryPropertyW(set, &data, SPDRP_HARDWAREID,
                nullptr, reinterpret_cast<PBYTE>(hwid), sizeof(hwid), nullptr)) {
            continue;
        }

        // SPDRP_HARDWAREID is REG_MULTI_SZ: check every null-terminated id,
        // not just the first, so a match on any of them counts.
        for (const wchar_t* p = hwid; *p; p += wcslen(p) + 1) {
            std::wstring id(p);
            for (wchar_t& c : id) c = towupper(c);   // case-insensitive contains

            if (id.find(vidTok) != std::wstring::npos &&
                id.find(pidTok) != std::wstring::npos) {
                if (auto port = ReadPortName(set, data)) {
                    result = port;
                }
                break;
            }
        }
    }

    SetupDiDestroyDeviceInfoList(set);
    return result;
}

DeviceManager::~DeviceManager() {
    ClosePort();
}

bool DeviceManager::OpenPort(const std::wstring& port) {
    ClosePort();

    // \\.\COMx form required for COM10+.
    std::wstring path = L"\\\\.\\" + port;
    handle_ = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                          OPEN_EXISTING, 0, nullptr);
    if (handle_ == INVALID_HANDLE_VALUE) {
        Log("WARN", "OpenPort CreateFile failed");
        return false;
    }

    DCB dcb = {};
    dcb.DCBlength = sizeof(dcb);
    if (GetCommState(handle_, &dcb)) {
        dcb.BaudRate = Config::BAUD;
        dcb.ByteSize = 8;
        dcb.Parity = NOPARITY;
        dcb.StopBits = ONESTOPBIT;
        SetCommState(handle_, &dcb);
    } else {
        Log("WARN", "GetCommState failed; port left unconfigured");
    }

    COMMTIMEOUTS to = {};
    to.WriteTotalTimeoutConstant = 500;
    SetCommTimeouts(handle_, &to);

    Log("INFO", "OpenPort ok");
    return true;
}

void DeviceManager::ClosePort() {
    if (handle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
        Log("INFO", "ClosePort");
    }
}
