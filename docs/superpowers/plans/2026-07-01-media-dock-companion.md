# MediaDockCompanion Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Background Windows tray app that pushes now-playing track info, system volume, and default audio device name to a USB-CDC music dock over a virtual COM port.

**Architecture:** Single-process Win32 app, one UI thread owning a hidden message window + message loop. Producer subsystems (USB device events, CoreAudio callbacks, SMTC WinRT callbacks) run on their own OS/WinRT threads; each writes the shared `StateStore` (mutex-guarded) then `PostMessage`s `WM_APP_STATE_CHANGED` to the UI window. The UI thread is the **only** writer to the COM handle — it serializes and writes on that message, so no cross-thread port access. A dirty flag prevents redundant writes.

**Tech Stack:** C++20, Win32 API, SetupAPI + RegisterDeviceNotification (USB/COM), Core Audio (`IMMDeviceEnumerator`/`IAudioEndpointVolume`), C++/WinRT SMTC (`GlobalSystemMediaTransportControlsSessionManager`), CMake + vcpkg + MSVC.

## Global Constraints

- Windows-only. Build with MSVC (`cl.exe`) + Ninja via `CMakePresets.json`. Cannot build in WSL.
- Target VID/PID are project constants — set them once in `Config.h` (placeholder until firmware VID/PID known).
- `WIN32_EXECUTABLE` (no console). Entry point is `main` via `/ENTRY:mainCRTStartup` (already in `CMakeLists.txt`) — for a windowed app switch this to a real `WinMain`; see Task 1.
- Runs forever in background: no leaks, all HANDLE/COM/WinRT objects released on their owner's teardown.
- Testing is **manual + log-driven** (this domain — tray, USB, audio, SMTC — has no practical unit harness). Every task's "verify" step reads `%LOCALAPPDATA%\MediaDockCompanion\log.txt`.
- Commit after each task. Repo is not yet git — Task 0 initializes it.

---

## File Structure

- `Config.h` — VID/PID constants, log path, baud, JSON field names.
- `Logger.h` / `Logger.cpp` — file logger, `Log(level, msg)`.
- `StateStore.h` / `StateStore.cpp` — `AppState` struct + mutex-guarded setters, dirty flag.
- `DeviceManager.h` / `DeviceManager.cpp` — COM-port scan, USB arrival/removal, port handle.
- `AudioMonitor.h` / `AudioMonitor.cpp` — Core Audio volume + default-device name + callbacks.
- `MediaMonitor.h` / `MediaMonitor.cpp` — SMTC session watcher + track callbacks.
- `Serializer.h` / `Serializer.cpp` — `SerializeState`, `WriteToDevice`.
- `Startup.h` / `Startup.cpp` — HKCU Run-key autostart toggle.
- `MediaDockCompanion.cpp` — `WinMain`, hidden window, message loop, tray icon/menu, subsystem wiring.

Shared UI-thread signal: `#define WM_APP_STATE_CHANGED (WM_APP + 1)`, `WM_APP_DEVICE_ARRIVED (WM_APP + 2)`, `WM_APP_DEVICE_REMOVED (WM_APP + 3)` (declared in `Config.h`).

---

## Task 0: Git + skeleton config

**Files:**
- Create: `.gitignore`, `Config.h`

**Interfaces:**
- Produces: `namespace Config { extern const wchar_t* LOG_PATH; constexpr USHORT VID=0xFFFF, PID=0xFFFF; constexpr DWORD BAUD=115200; }` and the `WM_APP_*` message ids.

- [ ] **Step 1:** `git init`. Create `.gitignore` with `out/`, `.vs/`, `vcpkg_installed/`, `*.exe`.
- [ ] **Step 2:** Create `Config.h` with the constants + `WM_APP_*` defines above (VID/PID as `0xFFFF` placeholders with a `// TODO: real firmware VID/PID` comment).
- [ ] **Step 3:** `git add -A && git commit -m "chore: git init + config skeleton"`

---

## Phase 1 — Tray app skeleton

### Task 1: WinMain + message loop + tray icon

**Files:**
- Modify: `MediaDockCompanion.cpp` (replace Hello-World `main`), `MediaDockCompanion.h`
- Modify: `CMakeLists.txt:20` — keep `/ENTRY:mainCRTStartup` OR change to standard `WinMain`; simplest: keep `int main()` and let it call the windowed logic (works with `WIN32_EXECUTABLE` + `mainCRTStartup`).
- Create: `Logger.h`, `Logger.cpp`

**Interfaces:**
- Produces: `void Log(const char* level, const std::string& msg);` (thread-safe, appends `[time][level] msg\n` to `Config::LOG_PATH`, opens/closes per call or holds one handle under a mutex).

- [ ] **Step 1:** Implement `Logger` — create `%LOCALAPPDATA%\MediaDockCompanion\` dir if missing, append lines with timestamp. Guard with `std::mutex`.
- [ ] **Step 2:** In `MediaDockCompanion.cpp`: register a window class, create a **message-only-ish hidden window** (normal `CreateWindowEx` with no `WS_VISIBLE`), add a tray icon via `Shell_NotifyIcon` (`NIM_ADD`), set `uCallbackMessage`.
- [ ] **Step 3:** Build a popup `HMENU` with "Show" and "Exit". On `WM_RBUTTONUP`/tray callback, `TrackPopupMenu`. "Exit" → `Shell_NotifyIcon(NIM_DELETE)` + `PostQuitMessage(0)`. "Show" → `Log("INFO","Show clicked")` (no UI yet).
- [ ] **Step 4:** Standard `GetMessage`/`TranslateMessage`/`DispatchMessage` loop. `Log("INFO","startup")` at launch.
- [ ] **Step 5 (verify):** `cmake --build --preset x64-debug`, run the exe. Confirm: tray icon appears, right-click shows Show/Exit, Exit removes icon and quits, `log.txt` has startup + click lines.
- [ ] **Step 6:** `git commit -m "feat: tray app skeleton + file logger"`

---

## Phase 2 — Device detection

### Task 2: COM-port scan + USB arrival/removal

**Files:**
- Create: `DeviceManager.h`, `DeviceManager.cpp`
- Modify: `MediaDockCompanion.cpp` (register device notification, handle `WM_DEVICECHANGE`, own a `DeviceManager`)

**Interfaces:**
- Consumes: `Config::VID/PID`, `Log`.
- Produces:
  - `std::optional<std::wstring> ScanForDevice(USHORT vid, USHORT pid);` — enumerates `GUID_DEVINTERFACE_COMPORT` via `SetupDiGetClassDevs`, reads each device's hardware ID, matches `VID_xxxx&PID_xxxx`, returns port name (`L"COM3"`).
  - `class DeviceManager { bool OpenPort(const std::wstring& port); void ClosePort(); bool IsConnected() const; HANDLE Handle() const; };` — `OpenPort` does `CreateFileW(\\.\COM3, GENERIC_WRITE, ...)`, sets `DCB` baud from `Config::BAUD`.

- [ ] **Step 1:** Implement `ScanForDevice` (SetupAPI enumerate + hardware-ID substring match). Log matches/misses.
- [ ] **Step 2:** Implement `DeviceManager::OpenPort`/`ClosePort`/`IsConnected` with logging. Idempotent close.
- [ ] **Step 3:** In `MediaDockCompanion.cpp`: `RegisterDeviceNotification` for `GUID_DEVINTERFACE_COMPORT` on the hidden window.
- [ ] **Step 4:** Handle `WM_DEVICECHANGE`: on `DBT_DEVICEARRIVAL` → `PostMessage(WM_APP_DEVICE_ARRIVED)`; on `DBT_DEVICEREMOVECOMPLETE` → `PostMessage(WM_APP_DEVICE_REMOVED)`. On arrival message: `ScanForDevice`→`OpenPort`. On removal: `ClosePort`.
- [ ] **Step 5 (verify):** Temporarily set `Config::VID/PID` to any COM device you own (e.g. a USB-serial adapter). Run, plug/unplug it. Confirm log shows scan hit, `OpenPort COM# ok`, then `ClosePort` on removal. No data sent yet.
- [ ] **Step 6:** `git commit -m "feat: USB detection + COM port open/close"`

---

## Phase 3 — CoreAudio volume + device name

### Task 3: Volume/device read + change callbacks

**Files:**
- Create: `AudioMonitor.h`, `AudioMonitor.cpp`, `StateStore.h`, `StateStore.cpp`
- Modify: `MediaDockCompanion.cpp` (CoInitializeEx, own `AudioMonitor` + `StateStore`)

**Interfaces:**
- Produces:
  - `struct AppState { int volume=-1; std::string deviceName, title, artist, playback; };`
  - `class StateStore { void SetVolume(int); void SetDeviceName(std::string); void SetTrack(std::string title,std::string artist,std::string playback); AppState Snapshot(); bool TakeDirty(); };` — every setter marks dirty under mutex; `TakeDirty` returns+clears the flag.
  - `class AudioMonitor { void Start(HWND notifyHwnd, StateStore* store); void Stop(); int GetCurrentVolume(); std::string GetCurrentDeviceName(); };` — implements `IAudioEndpointVolumeCallback::OnNotify` and `IMMNotificationClient::OnDefaultDeviceChanged`; each callback writes `store` then `PostMessage(notifyHwnd, WM_APP_STATE_CHANGED, 0, 0)`.

- [ ] **Step 1:** `CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)` in `WinMain` before subsystems; `CoUninitialize` on exit.
- [ ] **Step 2:** Implement `StateStore` (mutex + dirty flag).
- [ ] **Step 3:** Implement `AudioMonitor`: enumerate default render endpoint, `GetCurrentVolume` (0–100 from `GetMasterVolumeLevelScalar`), `GetCurrentDeviceName` (device `FriendlyName` property). Populate `StateStore` once at `Start`.
- [ ] **Step 4:** Register `IAudioEndpointVolumeCallback` + `IMMNotificationClient`. On `OnNotify` update volume; on `OnDefaultDeviceChanged` re-bind endpoint + update name. Both `PostMessage(WM_APP_STATE_CHANGED)`.
- [ ] **Step 5 (verify):** Run, change Windows volume + switch default output device. Confirm log lines show correct volume % and device name on each change.
- [ ] **Step 6:** `git commit -m "feat: coreaudio volume + device name callbacks"`

---

## Phase 4 — SMTC track info

### Task 4: Global media session watcher

**Files:**
- Create: `MediaMonitor.h`, `MediaMonitor.cpp`
- Modify: `MediaDockCompanion.cpp` (init_apartment, own `MediaMonitor`)
- Modify: `CMakeLists.txt` — ensure `Microsoft::CppWinRT` linked (already present).

**Interfaces:**
- Produces: `class MediaMonitor { void Start(HWND notifyHwnd, StateStore* store); void Stop(); };` — watches `GlobalSystemMediaTransportControlsSessionManager`; on any change writes title/artist/playback to `store` + `PostMessage(WM_APP_STATE_CHANGED)`.

- [ ] **Step 1:** `winrt::init_apartment()` (multithreaded) in `WinMain`. (Ensure it coexists with `CoInitializeEx` — use `init_apartment(apartment_type::multi_threaded)` OR call only WinRT init and let CoreAudio use MTA; document choice in code comment.)
- [ ] **Step 2:** `GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get()`; subscribe `SessionsChanged`.
- [ ] **Step 3:** On active session, subscribe `MediaPropertiesChanged` + `PlaybackInfoChanged`. Read `TryGetMediaPropertiesAsync().get()` → title/artist; `GetPlaybackInfo().PlaybackStatus()` → "Playing"/"Paused"/"Stopped".
- [ ] **Step 4:** Edge cases: no active session → set empty track + playback "None"; session closes mid-play → revoke handlers, clear state. Wrap WinRT calls in try/catch, `Log` on `winrt::hresult_error`.
- [ ] **Step 5 (verify):** Play/pause/skip in Spotify, YouTube (browser), Windows Media Player. Confirm log shows correct title/artist/state for all three, and empty/None when nothing plays.
- [ ] **Step 6:** `git commit -m "feat: SMTC track info watcher"`

---

## Phase 5 — Serializer + writer

### Task 5: JSON serialize + COM write on dirty

**Files:**
- Create: `Serializer.h`, `Serializer.cpp`
- Modify: `MediaDockCompanion.cpp` (`WM_APP_STATE_CHANGED` handler)

**Interfaces:**
- Consumes: `AppState`, `DeviceManager::Handle/IsConnected`, `StateStore::TakeDirty/Snapshot`.
- Produces:
  - `std::string SerializeState(const AppState& s);` — hand-built JSON: `{"vol":50,"dev":"...","title":"...","artist":"...","play":"Playing"}` with string escaping for `"` and `\`.
  - `bool WriteToDevice(HANDLE h, const std::string& payload);` — `WriteFile(payload + "\n")`; returns false on failure.

- [ ] **Step 1:** Implement `SerializeState` with minimal JSON string escaping (`"`, `\`, control chars).
- [ ] **Step 2:** Implement `WriteToDevice` (append `\n`, `WriteFile`, check bytes written).
- [ ] **Step 3:** In `WM_APP_STATE_CHANGED`: `if (deviceMgr.IsConnected() && store.TakeDirty()) { write = WriteToDevice(...SerializeState(store.Snapshot())); if(!write){ Log("WARN","write failed, assuming disconnect"); deviceMgr.ClosePort(); } }`. Also trigger one send on `WM_APP_DEVICE_ARRIVED` (force dirty).
- [ ] **Step 4 (verify):** Flash STM32 (or a serial-loopback / PuTTY on the COM port) to echo. Change volume/track. Confirm exactly one JSON line per change appears at the receiver, `\n`-terminated, escapes correct.
- [ ] **Step 5:** `git commit -m "feat: json serializer + com writer with dirty flag"`

---

## Phase 6 — Firmware integration test (no new code)

### Task 6: End-to-end with real device

- [ ] **Step 1:** Set real `Config::VID/PID`. Connect real dock.
- [ ] **Step 2:** Verify packets arrive at STM32; display updates on track change, volume change, device switch.
- [ ] **Step 3:** Confirm no duplicate sends (dirty flag) and no missed events (each Windows change → one line).
- [ ] **Step 4:** Stress: rapidly skip tracks + spin volume. Watch for dropped/garbled lines; note any coalescing needed (fold into Phase 8 if found).
- [ ] **Step 5:** `git commit -m "test: firmware integration verified" --allow-empty` (record results in commit body).

---

## Phase 7 — Startup + persistence

### Task 7: Autostart toggle

**Files:**
- Create: `Startup.h`, `Startup.cpp`
- Modify: `MediaDockCompanion.cpp` (menu item + checkmark)

**Interfaces:**
- Produces: `bool IsAutostartEnabled(); void SetAutostart(bool);` — read/write `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` value `MediaDockCompanion` = exe path.

- [ ] **Step 1:** Implement registry read/write via `RegSetValueExW`/`RegQueryValueExW`; value = full exe path from `GetModuleFileNameW`.
- [ ] **Step 2:** Add "Start with Windows" tray menu item; show check (`MF_CHECKED`) reflecting `IsAutostartEnabled`; toggle on click.
- [ ] **Step 3 (verify):** Enable toggle, reboot. Confirm app starts, tray appears, device connects, display updates. Confirm menu check state persists.
- [ ] **Step 4:** `git commit -m "feat: HKCU Run autostart toggle"`

> Optional (defer unless needed): Task Scheduler USB-arrival trigger as secondary launch. Not required if Run-key autostart + always-resident app is sufficient. Skip until a concrete need appears (YAGNI).

---

## Phase 8 — Polish + stability

### Task 8: Hardening

**Files:**
- Modify: `MediaMonitor.cpp`, `DeviceManager.cpp`, `MediaDockCompanion.cpp`

- [ ] **Step 1:** Wrap all SMTC async reads in try/catch for `winrt::hresult_error` (session vanishing mid-query) — log + skip, never crash.
- [ ] **Step 2:** Reconnect backoff: if `OpenPort` fails on arrival, `SetTimer` 2000 ms and retry on `WM_TIMER`, not immediately. Cancel timer once open.
- [ ] **Step 3:** Memory/handle audit — confirm every COM/WinRT/HANDLE released; no per-callback allocations that leak. Prefer reused buffers.
- [ ] **Step 4 (verify):** Leave running 24 h with device connected. Check Task Manager handle/memory count stable; `log.txt` shows no unhandled errors.
- [ ] **Step 5:** `git commit -m "feat: hardening — async guards, reconnect backoff, leak fixes"`

---

## Self-review notes

- Every spec phase (1–8) maps to Task 1–8; Task 0 adds git+config the spec assumes.
- "Trigger send" from the spec is centralized on the UI thread via `WM_APP_STATE_CHANGED` + dirty flag — avoids the spec's implicit cross-thread COM-write hazard.
- Signatures consistent across tasks: `StateStore`, `DeviceManager`, `AudioMonitor`, `MediaMonitor`, `SerializeState`/`WriteToDevice` referenced with the same names/types where they appear.
- WinRT MTA vs CoreAudio apartment coexistence flagged in Task 4 Step 1 — the one real integration risk to watch.
