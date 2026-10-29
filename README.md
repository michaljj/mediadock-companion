# MediaDockCompanion

Windows tray application for the [MediaDock](https://github.com/michaljj/MediaDock) USB-C dock.
It sits in the notification area and pushes three things to the dock's display over a
virtual COM port, a few times per second:

- **now playing** — track, artist and playback state from Windows' System Media Transport Controls
- **system volume** — including mute state
- **default audio output device** — the name of whatever endpoint is currently selected

The dock is a USB CDC device; this app is the host half. Firmware lives with the
[STM32 dock projects](https://github.com/michaljj/STM32VolumeDockRTOS).

---

## How it works

A background tray process with no main window. One message loop, one worker per concern:

| Component | Responsibility |
|---|---|
| `MediaMonitor` | SMTC session events on a dedicated MTA thread |
| `AudioMonitor` | Core Audio volume + default-endpoint callbacks, marshalled to the UI thread |
| `DeviceManager` | COM-port enumeration, open, and reconnect with backoff |
| `StateStore` | mutex-guarded snapshot + dirty flag shared between threads |
| `Serializer` | one JSON line per update, written to the COM port |
| `Startup` | HKCU `Run` autostart toggle |
| `Config`, `Logger`, `Util` | settings, rotating log, helpers |

The threads never talk directly: producers mark the snapshot dirty, and a single
writer drains it. That keeps the serial link from receiving interleaved partial
writes when a track change and a volume change land in the same millisecond.

Windows libraries linked (`MMDevAPI`, `HID`, `SetupAPI`, `Comctl32`, C++/WinRT
projection) are the tell for what the app touches: Core Audio, USB/HID discovery,
and the native Win32 shell.

The target is `WIN32_EXECUTABLE` with the entry point overridden to `mainCRTStartup`,
so `main()` — not `WinMain` — is the entry point, and no console window appears.

---

## Build

Windows only — MSVC, C++20, Ninja, and vcpkg in manifest mode (`vcpkg.json`).
Build from a Visual Studio 2022 developer environment, not WSL.

```powershell
cmake --preset x64-debug
cmake --build --preset x64-debug
```

Other presets: `x64-release`, `x86-debug`, `x86-release`. Output lands in
`out/build/<preset>/`. `vcpkg_installed/`, `out/` and `*.exe` are gitignored —
this repo is source only.

---

## Protocol

One JSON object per line, `\n`-terminated, written to the COM port:

```json
{"vol":42,"mute":false,"dev":"Speakers (Realtek)","title":"...","artist":"...","play":"playing"}
```

Strings are escaped by hand in `Serializer.cpp`, so a track title containing quotes
or newlines cannot break the line framing. The firmware parses line-delimited JSON
and updates the display; a dropped line beats a torn one.

---

## Status

Runs on the bench against a dock prototype. Reconnect backoff and async guards are
in place, so unplugging the dock does not take the tray process down with it. There
is no test suite or CI yet — see `CLAUDE.md` for the current state of play.

---

## License

Licensed under the [PolyForm Noncommercial License 1.0.0](LICENSE.md). Free for
personal, academic and other noncommercial use. Commercial use requires a separate
license — get in touch.
