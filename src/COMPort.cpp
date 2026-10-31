#include "COMPort.hpp"
#include <windows.h>
#include <devguid.h>
#include <setupapi.h>
#include <array>
#include <cstdlib>
#include <stdexcept>
#include <string>

HANDLE openSerialPort (const std::wstring &portName)
{
  const std::wstring path = LR"(\\.\)" + portName;

  HANDLE h = CreateFileW (path.c_str (), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);

  if (h == INVALID_HANDLE_VALUE)
    {
      throw std::runtime_error ("Could not open serial port, Win32 error: " + std::to_string (GetLastError ()));
    }

  DCB dcb{};
  dcb.DCBlength = sizeof (dcb);

  if (!GetCommState (h, &dcb))
    {
      CloseHandle (h);
      throw std::runtime_error ("GetCommState failed");
    }

  dcb.BaudRate = CBR_115200;
  dcb.ByteSize = 8;
  dcb.Parity = NOPARITY;
  dcb.StopBits = ONESTOPBIT;
  dcb.fBinary = TRUE;
  dcb.fParity = FALSE;
  // Explicit because AGENTS.md calls this flag out: with fAbortOnError == TRUE
  // a single comm error poisons every later write (Win32 err 22).
  dcb.fAbortOnError = FALSE;

  // GetCommState returns whatever the driver / a previous opener left
  // configured, so handshake flags must be cleared explicitly. If hardware
  // flow control survived here and the dock (3-wire link: TX/RX/GND, firmware
  // never drives CTS/DSR) does not assert CTS, every WriteFile would stall
  // and die with ERROR_SEM_TIMEOUT (121) - a host-side config cause that
  // would be indistinguishable from a dock RX stall in the logs.
  dcb.fOutxCtsFlow = FALSE;
  dcb.fOutxDsrFlow = FALSE;
  dcb.fDsrSensitivity = FALSE;
  dcb.fDtrControl = DTR_CONTROL_ENABLE;
  dcb.fRtsControl = RTS_CONTROL_ENABLE;
  dcb.fOutX = FALSE;
  dcb.fInX = FALSE;
  dcb.fNull = FALSE;
  dcb.fErrorChar = FALSE;

  if (!SetCommState (h, &dcb))
    {
      CloseHandle (h);
      throw std::runtime_error ("SetCommState failed");
    }

  // Write deadline: 100 ms + 10 ms per byte. The wire itself needs ~87 us
  // per byte at 115200, so this is >100x slack. A WriteFile that still
  // expires with ERROR_SEM_TIMEOUT (121) therefore means the device accepted
  // no (or not enough) bytes in time: the far end is not draining its RX.
  COMMTIMEOUTS t{};
  t.ReadIntervalTimeout = 50;
  t.ReadTotalTimeoutConstant = 100;
  t.ReadTotalTimeoutMultiplier = 10;
  t.WriteTotalTimeoutConstant = 100;
  t.WriteTotalTimeoutMultiplier = 10;

  if (!SetCommTimeouts (h, &t))
    {
      CloseHandle (h);
      throw std::runtime_error ("SetCommTimeouts failed");
    }

  return h;
}
// Parse "USB Serial Device (COM9)" / "STMicroelectronics ... (COM8)" style
// friendly names. The port name is always the last parenthesised token.
static std::wstring portFromFriendlyName (const wchar_t *name)
{
  if (name == nullptr)
    {
      return {};
    }
  const std::wstring      text{ name };
  const std::wstring_view open{ L"(COM" };
  const size_t            start = text.rfind (open);
  if (start == std::wstring::npos)
    {
      return {};
    }
  const size_t close = text.find (L')', start);
  if (close == std::wstring::npos)
    {
      return {};
    }
  return text.substr (start + 1U, close - start - 1U);
}

[[nodiscard]] std::wstring findDockPort ()
{
  std::array<wchar_t, 64> envPort{};
  const DWORD             envLen
      = GetEnvironmentVariableW (L"MEDIADOCK_COM_PORT", envPort.data (), static_cast<DWORD> (envPort.size ()));
  if ((envLen > 0U) && (envLen < envPort.size ()))
    {
      return envPort.data ();
    }

  HDEVINFO devs = SetupDiGetClassDevsW (&GUID_DEVCLASS_PORTS, nullptr, nullptr, DIGCF_PRESENT);
  if (devs == INVALID_HANDLE_VALUE)
    {
      return {};
    }
  std::wstring found;
  SP_DEVINFO_DATA info{};
  info.cbSize = sizeof (info);
  for (DWORD i = 0; SetupDiEnumDeviceInfo (devs, i, &info); ++i)
    {
      std::array<wchar_t, 512> hwid{};
      if (!SetupDiGetDeviceRegistryPropertyW (devs, &info, SPDRP_HARDWAREID, nullptr, reinterpret_cast<PBYTE> (hwid.data ()),
                                              static_cast<DWORD> (hwid.size () * sizeof (wchar_t)), nullptr))
        {
          continue;
        }
      // TinyUSB descriptors: VID 0xCAFE, PID 0x4005 (see usb_descriptors.c).
      if (wcsstr (hwid.data (), L"VID_CAFE&PID_4005") == nullptr)
        {
          continue;
        }
      std::array<wchar_t, 256> friendly{};
      if (!SetupDiGetDeviceRegistryPropertyW (devs, &info, SPDRP_FRIENDLYNAME, nullptr, reinterpret_cast<PBYTE> (friendly.data ()),
                                              static_cast<DWORD> (friendly.size () * sizeof (wchar_t)), nullptr))
        {
          continue;
        }
      found = portFromFriendlyName (friendly.data ());
      if (!found.empty ())
        {
          break;
        }
    }
  SetupDiDestroyDeviceInfoList (devs);
  return found;
}
