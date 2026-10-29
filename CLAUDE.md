# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project state

MediaDockCompanion is a **Windows C++20 desktop application**: a background tray app that pushes now-playing track info, system volume, and default audio device name to a USB-CDC dock over a virtual COM port. It is implemented across `MediaDockCompanion.cpp` (entry, hidden message window, tray, message loop, wiring) plus subsystems: `DeviceManager` (COM-port scan/open), `AudioMonitor` (Core Audio volume + device via callbacks marshalled to the UI thread), `MediaMonitor` (SMTC on its own MTA thread), `StateStore` (mutex-guarded snapshot + dirty flag), `Serializer` (JSON line + COM write), `Startup` (HKCU Run autostart), `Logger`, `Config`, `Util`. See `docs/superpowers/plans/` for the implementation plan.

Linked Windows libraries (see `CMakeLists.txt`) signal the app's planned scope:
- `mmdevapi` — audio endpoint / device enumeration (Core Audio)
- `hid`, `setupapi` — HID / USB device discovery (the "dock" hardware)
- `comctl32` — native Win32 common controls (UI)
- `Microsoft::CppWinRT` (`cppwinrt`) — WinRT projection for modern Windows APIs

The target is a `WIN32_EXECUTABLE` (no console window) with entry point overridden to `mainCRTStartup`, so `main()` — not `WinMain` — is the entry point.

## Build

**This project builds on Windows only** (MSVC `cl.exe` + Ninja + vcpkg). The repo lives on a Windows drive but this shell is WSL; `cl.exe` is not available here. Build from a Windows Developer environment (e.g. Visual Studio 2022 Developer Command Prompt / PowerShell), not from WSL.

vcpkg is in manifest mode (`vcpkg.json`), toolchain resolved via the preset. Presets are defined in `CMakePresets.json`: `x64-debug`, `x64-release`, `x86-debug`, `x86-release`.

```powershell
cmake --preset x64-debug        # configure (fetches vcpkg deps, generates Ninja build)
cmake --build --preset x64-debug
```

If `--build --preset` is unavailable, build the generated dir directly:
```powershell
cmake --build out/build/x64-debug
```

Build output lands in `out/build/<preset>/`. The vcpkg toolchain path is hardcoded in `CMakePresets.json` to the VS 2022 Community install — adjust `CMAKE_TOOLCHAIN_FILE` if vcpkg lives elsewhere.

## Notes

- No test suite, linter, or CI is configured yet.
- Git repository (`.git/` present).
- The `MediaDockCompanion.exe` at the repo root is a 0-byte placeholder, not a build artifact — real output is under `out/build/`.
