#pragma once

#include <windows.h>
#include <string>

HANDLE openSerialPort (const std::wstring &portName);

// Locate the dock's TinyUSB CDC port by USB VID/PID (VID_CAFE&PID_4005).
// Returns e.g. L"COM9", or an empty string when no dock is attached.
// MEDIADOCK_COM_PORT overrides detection when set (useful for UART bridges).
[[nodiscard]] std::wstring findDockPort ();