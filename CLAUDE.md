# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project state

MediaDockCompanion is a **Windows C++20 desktop application** scaffold. `MediaDockCompanion.cpp` is currently a Hello-World stub (`main()` prints and returns). The real application has not been written yet — treat the linked libraries as the intended feature surface, not as existing code.

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
- Not a git repository.
- The `MediaDockCompanion.exe` at the repo root is a 0-byte placeholder, not a build artifact — real output is under `out/build/`.
