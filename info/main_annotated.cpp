// =====================================================================================
// MediaDock companion app — ANNOTATED LEARNING COPY
// =====================================================================================
//
// This file is a heavily commented copy of src/main.cpp. It is NOT compiled
// (see CMakeLists.txt: only src/main.cpp, serializer.cpp, COMPort.cpp are in
// the build). Its only purpose is teaching: every section explains WHAT the
// code does, HOW the underlying Windows API works, and WHY it is written
// this way. When main.cpp changes, this copy will drift — treat it as a
// snapshot for learning, not a source of truth.
//
// The big picture first:
//
//   [Spotify / Edge / Chrome / ...]          (apps implement SMTC themselves)
//              |  (in-process, no copy)
//              v
//   [Windows SMTC "media hub"]   <---- this app reads from here
//              |
//              v
//   [main.cpp: session manager]  --events--> print to console
//              |                            + SendToDock()
//              v
//   [serializer.hpp]  TLV payload -> COBS encode -> 0x00 frame
//              |
//              v
//   [COM4 serial port]  -----------------> STM32 dock decodes & shows on LCD
//
// Two completely separate Windows subsystems are used:
//   1. SMTC  (System Media Transport Controls) — "what is playing, where"
//   2. MMDevice (Core Audio)                   — "what speakers exist"
// Both are exposed as COM/WinRT APIs that ship inside the Windows SDK.
//
// =====================================================================================

// -------------------------------------------------------------------------------------
// INCLUDE ORDER IS LOAD-BEARING — do not reorder without testing.
// -------------------------------------------------------------------------------------

// windows.h MUST come first. It drags in the whole Win32 surface and sets up
// Windows-specific macros. Two traps it creates:
//   - It defines min()/max() as MACROS. Any later use of std::min/std::max in
//     headers reachable from here must be written (std::min) with parens to
//     stop macro expansion. This broke the build once already.
//   - It defines the Win32 base types (HANDLE, DWORD, HRESULT, ...) that
//     everything below uses.
#include <windows.h>

// C++/WinRT projection headers. These are machine-generated C++ wrappers over
// Windows Runtime (WinRT) API classes. "Projection" means: the real API lives
// inside the OS (in COM-style objects activated at runtime), and these headers
// just give you a nice C++ type for it (smart pointers, exceptions, events).
// Nothing to link beyond windowsapp.lib (see the #pragma comment below).
//
// Ordering rule: Windows.ApplicationModel.h before Windows.Management.Deployment.h,
// so Package::Id() is fully defined when PackageManager code below uses it.
#include <winrt/Windows.ApplicationModel.h>       // Package info (app names)
#include <winrt/Windows.Foundation.Collections.h> // IVectorView etc. (GetSessions returns one)
#include <winrt/Windows.Foundation.h>             // hstring, IAsyncOperation, event, ...
#include <winrt/Windows.Management.Deployment.h>  // PackageManager (AUMID -> app name)
#include <winrt/Windows.Media.Control.h>          // SMTC: THE media session API
#include <winrt/Windows.Storage.Streams.h>        // Streams for reading album art bytes

// Core Audio / MMDevice API. This is classic COM (not WinRT), so it uses the
// older header style. mmdeviceapi.h must come first: it (via propsys.h ->
// propkeydef.h) DEFINES the DEFINE_PROPERTYKEY macro that
// functiondiscoverykeys_devpkey.h only USES (that header's own devpropdef.h
// include is commented out inside the SDK — a known SDK quirk).
#include <mmdeviceapi.h>                    // IMMDeviceEnumerator, IMMDevice
#include <functiondiscoverykeys_devpkey.h>  // PKEY_Device_FriendlyName property key

// _setmode/_fileno: switch the C runtime's stdout to UTF-8 text mode so
// unicode titles (Japanese, emoji, ...) print correctly on the console.
#include <fcntl.h>
#include <io.h>

// C++ standard library bits used below.
#include <format>       // std::format for the [HH:MM:SS] timestamps
#include <fstream>      // std::ofstream to write thumbnail files
#include <iostream>     // std::wcout
#include <mutex>        // std::mutex, std::scoped_lock (serial send serialization)
#include <string>       // std::string, std::wstring
#include <string_view>  // non-owning string parameters
#include <vector>       // the session watcher list

// Our own protocol + port modules.
#include "COMPort.hpp"    // openSerialPort(): opens + configures a COM port
#include "serializer.hpp" // serializer class: TLV payload + COBS framing

// #pragma comment(lib, ...) tells MSVC to link these libraries without
// needing to list them in CMake target_link_libraries.
#pragma comment(lib, "windowsapp.lib") // All WinRT (Windows.*) API implementations
#pragma comment(lib, "ole32.lib")      // CoCreateInstance and COM plumbing

// Using-directives to keep SMTC type names readable below. In a header this
// would be bad practice; in the single .cpp that owns everything, it is fine.
using namespace winrt;
using namespace Windows::Media::Control;

// =====================================================================================
// SECTION 1: THE SERIAL LINK ("to the dock")
// =====================================================================================
//
// Data flow out of the app:
//
//   media metadata (UTF-16 in Windows, UTF-8 on the wire)
//        |
//        v
//   serializer: 5 fields -> TLV payload -> COBS encode -> frame + 0x00
//        |                  (see inc/serializer.hpp for the byte format)
//        v
//   WriteFile(COM4)  ->  STM32 dock's UART -> firmware decoder (future)
//
// Why a mutex? SMTC events arrive on WINDOWS THREADPOOL threads — not on our
// main thread. Two media events firing close together means two callbacks can
// run SendToDock() simultaneously. The serializer object and the port write
// are shared state; interleaving them would corrupt frames (half of track A
// followed by half of track B on the wire). One mutex around the ENTIRE
// send (encode + write, not just one of them) makes every frame atomic.

static winrt::handle g_comPort; // RAII wrapper around a HANDLE: closes itself
                                 // when destroyed, bool-tests as "is open"
static serializer    g_serializer; // builds the wire frame, see serializer.hpp

static std::mutex g_serialMutex; // guards g_comPort + g_serializer + the write

// Send one complete metadata frame to the dock.
// Callable from any thread. Does nothing (by design) if the dock is not
// attached, so media watching keeps working headless.
static void SendToDock (std::wstring_view app, std::wstring_view title, std::wstring_view artist,
                        std::wstring_view album, std::string_view playbackStatus)
{
  // scoped_lock: like lock_guard but tidy prefers it; unlocks on scope exit
  // even if an exception is thrown (RAII lock). Held for the whole function:
  // the frame must be encoded AND written as one atomic unit.
  std::scoped_lock const lock (g_serialMutex);

  // No port == no dock attached. This is a NORMAL situation (you can run the
  // app on any PC to watch media without hardware), so just return silently.
  if (!g_comPort)
    {
      return;
    }

  // Surface and clear any stale error state in the serial driver. Without
  // this, an old error (e.g. from unplugging the dock) can poison later
  // writes. We only LOG the mask here; ClearCommError already cleared it.
  DWORD commErr = 0;
  ClearCommError (g_comPort.get (), &commErr, nullptr);
  if (commErr)
    {
      std::wcout << L"serial: comm error mask 0x" << std::hex << commErr << std::dec << L"\n";
    }

  // Windows strings are UTF-16 (wchar_t). The wire format is UTF-8. Convert
  // here, INSIDE the port check, so headless runs pay zero conversion cost
  // per event. winrt::to_string(hstring-ish) = UTF-16 -> UTF-8.
  g_serializer.setApp (winrt::to_string (app));
  g_serializer.setTitle (winrt::to_string (title));
  g_serializer.setArtist (winrt::to_string (artist));
  g_serializer.setAlbum (winrt::to_string (album));
  g_serializer.setPlaybackStatus (playbackStatus); // already UTF-8 (narrow literals)

  // encode() = build TLV payload -> COBS encode -> frame it (preamble +
  // 3-byte length + COBS block + 0x00 delimiter).
  // Returns the exact number of bytes to write. bufferValid() is false only
  // if something went catastrophically wrong; belt-and-braces check.
  size_t const frameSize = g_serializer.encode ();
  if (!g_serializer.bufferValid ())
    {
      return;
    }

  // The actual write. WriteFile works on a serial handle just like a file.
  DWORD written = 0;
  if (!WriteFile (g_comPort.get (), g_serializer.buffer ().data (),
                  static_cast<DWORD> (frameSize), &written, nullptr))
    {
      // FALSE return = hard failure (port gone, driver error...).
      std::wcout << L"serial: write failed, err=" << GetLastError () << L"\n";
    }
  else if (static_cast<size_t> (written) != frameSize)
    {
      // SUBTLE: COMPort.cpp sets write timeouts. With timeouts, WriteFile can
      // return TRUE having sent only SOME bytes. If we logged that as success,
      // a silently-truncated frame would reach the dock and the receiver would
      // lose fields. So check the count explicitly.
      std::wcout << L"serial: partial write, " << written << L"/" << frameSize << L" bytes\n";
    }
  else
    {
      std::wcout << L"serial: sent frame, " << written << L" bytes\n";
    }
}

// =====================================================================================
// SECTION 2: SMALL HELPERS
// =====================================================================================

// Returns "[14:23:05] " style timestamp. std::chrono::zoned_time + std::format
// (C++20) does timezone-aware formatting in one line; %H:%M:%S is the format.
static std::wstring Timestamp ()
{
  return std::format (L"[{:%H:%M:%S}] ",
                      std::chrono::zoned_time{ std::chrono::current_zone (), std::chrono::system_clock::now () });
}

// Map the SMTC playback-status enum to a printable (and wire-usable) name.
// Returns narrow const char* — the same string serves both the console (wcout
// handles narrow chars fine) and the serializer (which wants UTF-8).
static const char *PlaybackStatusName (GlobalSystemMediaTransportControlsSessionPlaybackStatus s)
{
  switch (s)
    {
    case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Closed:
      return "Closed";
    case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Opened:
      return "Opened";
    case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Changing:
      return "Changing";
    case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Stopped:
      return "Stopped";
    case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing:
      return "Playing";
    case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Paused:
      return "Paused";
    default:
      return "Unknown";
    }
}

// Resolve a SourceAppUserModelId (AUMID) into a friendly app name.
//
// Windows reports the media SOURCE as an "AUMID" (Application User Model ID),
// and there are two shapes:
//
//   Packaged (Store/UWP) apps:
//     "Microsoft.ZuneMusic_8wekyb3d8bbwe!Microsoft.ZuneMusic"
//      \__________ family (publisher hash included) ____/
//      The part before '!' identifies the package. We look it up with
//      PackageManager to get the nice display name ("Media Player").
//
//   Classic Win32 apps (Chrome, Spotify desktop...):
//     "chrome.exe" or "Spotify" — already usable as-is.
static std::wstring FriendlyAppName (std::wstring const &aumid)
{
  // find() with init-statement: the '!' separator only exists in packaged IDs.
  if (auto bang = aumid.find (L'!'); bang != std::wstring::npos)
    {
      std::wstring family = aumid.substr (0, bang);
      try
        {
          Windows::Management::Deployment::PackageManager pm;
          auto pkg = pm.FindPackageForUser (L"", family); // L"" = current user
          if (pkg)
            {
              return std::wstring{ pkg.Id ().Name () }; // e.g. "Microsoft.ZuneMusic"
            }
        }
      catch (...)
        { /* fall through: package lookup can fail (uninstalled mid-flight,
             permissions); showing the raw family name is still useful */
        }
      return family;
    }
  return aumid; // Win32 app: already friendly enough
}

// =====================================================================================
// SECTION 3: AUDIO ENDPOINTS ("on what speaker") — classic COM / MMDevice API
// =====================================================================================
//
// This is the OLD-STYLE COM API (predates WinRT by ~15 years). Differences to
// note while reading:
//   - You get raw interfaces via CoCreateInstance, wrapped in winrt::com_ptr
//     (a smart pointer that AddRef/Release's automatically).
//   - Methods return HRESULT error codes, not exceptions. SUCCEEDED/FAILED
//     macros check them.
//   - out-parameters are filled via .put() on the com_ptr (address of the
//     internal pointer).

// One device -> its human-readable name ("Speakers (Realtek Audio)").
// Device properties live in an IPropertyStore; the friendly name is stored
// under the well-known key PKEY_Device_FriendlyName.
static std::wstring GetDeviceFriendlyName (IMMDevice *device)
{
  std::wstring name = L"<unknown>"; // pessimistic default
  winrt::com_ptr<IPropertyStore> store; // com_ptr auto-Releases on scope exit
  if (SUCCEEDED (device->OpenPropertyStore (STGM_READ, store.put ())))
    {
      // PROPVARIANT is COM's "any-type" value (like a tagged union). MUST be
      // initialized with PropVariantInit and freed with PropVariantClear,
      // or you leak the string it holds.
      PROPVARIANT var;
      PropVariantInit (&var);
      // Triple check: call succeeded AND type is what we expect (VT_LPWSTR =
      // pointer to wide string) AND pointer non-null. Reading a PROPVARIANT
      // union member of the wrong type reads garbage.
      if (SUCCEEDED (store->GetValue (PKEY_Device_FriendlyName, &var)) && var.vt == VT_LPWSTR && var.pwszVal)
        {
          name = var.pwszVal;
        }
      PropVariantClear (&var);
    }
  return name;
}

// Lists the default render device plus every active render device (speakers,
// headphones, HDMI, Bluetooth, ...). Purely informational startup output.
static void ListSpeakers ()
{
  // CoCreateInstance = "activate a COM object by class ID". MMDeviceEnumerator
  // is the single system object that lists audio devices. check_hresult
  // throws on failure (we're in main, dying here is fine).
  winrt::com_ptr<IMMDeviceEnumerator> enumerator;
  winrt::check_hresult (CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                          __uuidof (IMMDeviceEnumerator), enumerator.put_void ()));

  // The device NEW audio would play on right now (what the user last picked).
  winrt::com_ptr<IMMDevice> defaultDevice;
  if (SUCCEEDED (enumerator->GetDefaultAudioEndpoint (eRender, eMultimedia, defaultDevice.put ())))
    {
      std::wcout << L"  Default speaker : " << GetDeviceFriendlyName (defaultDevice.get ()) << L"\n";
    }

  // Every render endpoint that is currently ACTIVE (plugged in + enabled).
  winrt::com_ptr<IMMDeviceCollection> collection;
  if (FAILED (enumerator->EnumAudioEndpoints (eRender, DEVICE_STATE_ACTIVE, collection.put ())))
    {
      return; // enumeration failed; not fatal, skip the list
    }

  UINT count = 0;
  collection->GetCount (&count);
  for (UINT i = 0; i < count; ++i)
    {
      winrt::com_ptr<IMMDevice> device;
      if (FAILED (collection->Item (i, device.put ())))
        {
          continue; // one dead device shouldn't kill the list
        }
      std::wcout << L"  Active endpoint : " << GetDeviceFriendlyName (device.get ()) << L"\n";
    }
}

// =====================================================================================
// SECTION 4: MEDIA SESSIONS ("what is playing") — SMTC, the modern WinRT API
// =====================================================================================
//
// Mental model: every app that shows in the Windows media flyout publishes
// ONE "session". The SessionManager is the hub that lists them. A session
// gives you: media properties (title/artist/album/thumbnail), playback info
// (status, controls), timeline (position), and CHANGE EVENTS.
//
// Events fire on THREADPOOL threads. That is why SendToDock has a mutex and
// why every session access is wrapped in try/catch — the app that owns the
// session can vanish at ANY moment between our API calls.

// Saves album art to "<base>.png" / "<base>.jpg" in the working directory.
static void SaveThumbnail (Windows::Storage::Streams::IRandomAccessStreamReference const &thumbRef,
                           std::wstring const                                            &baseName)
{
  if (!thumbRef) // session had no artwork at all
    {
      return;
    }

  // WinRT async pattern: every *Async() returns IAsyncOperation. Calling
  // .get() blocks until it completes. Safe here ONLY because we run in a
  // multi-threaded apartment (MTA) — in an STA UI thread, .get() on an async
  // that needs that same thread = classic deadlock.
  auto stream = thumbRef.OpenReadAsync ().get ();
  if (stream.Size () == 0)
    {
      return;
    }

  // Read the whole stream into memory in one shot: LoadAsync pulls bytes
  // from the stream into the reader's internal buffer, ReadBytes copies
  // them into our array. Hoist Size() once — it is a sync call per use.
  auto const byteCount = static_cast<uint32_t> (stream.Size ());
  Windows::Storage::Streams::DataReader reader (stream);
  reader.LoadAsync (byteCount).get ();
  winrt::com_array<uint8_t> bytes (byteCount);
  reader.ReadBytes (bytes);

  // Content type comes from the source; PNG is the common default.
  std::wstring ext = L".png";
  if (stream.ContentType () == L"image/jpeg")
    ext = L".jpg";

  // MSVC's std::ofstream accepts wide (std::wstring) paths — a MSVC extension.
  std::ofstream file ((baseName + ext).c_str (), std::ios::binary);
  if (file)
    {
      // bytes is uint8_t; ofstream wants char. reinterpret_cast is the one
      // legitimate tool for "same bytes, different pointer type".
      file.write (reinterpret_cast<char const *> (bytes.data ()), static_cast<std::streamsize> (bytes.size ()));
      std::wcout << L"  Thumbnail       : saved " << baseName << ext << L" (" << bytes.size () << L" bytes)\n";
    }
}

// Prints everything we know about one media session and forwards it to the
// dock. saveThumbnail only matters on the initial enumeration; on live events
// the thumbnail never changes, so re-saving it would just spam the disk.
static void DescribeSession (GlobalSystemMediaTransportControlsSession const &session, bool saveThumbnail)
{
  // TryGet (vs Get): won't throw if the session is already gone — returns an
  // async that yields null. .get() blocks a threadpool thread; safe because
  // this app is MTA and the async completes on the threadpool (see above).
  auto properties   = session.TryGetMediaPropertiesAsync ().get ();
  auto playbackInfo = session.GetPlaybackInfo ();

  // Copy everything out FIRST, then print/send. The session can die at any
  // moment; holding hstrings we already copied is safer than re-querying.
  std::wstring const app = FriendlyAppName (std::wstring{ session.SourceAppUserModelId () });
  auto         title  = properties.Title ();  // winrt::hstring (UTF-16, ref-counted)
  auto         artist = properties.Artist ();
  auto         album  = properties.AlbumTitle ();
  const char  *status = PlaybackStatusName (playbackInfo.PlaybackStatus ());

  std::wcout << L"  App             : " << app << L"\n";
  std::wcout << L"  Title           : " << title << L"\n";
  std::wcout << L"  Artist          : " << artist << L"\n";
  std::wcout << L"  Album           : " << album << L"\n";
  std::wcout << L"  Playback status : " << status << L"\n";

  // The wire format is UTF-8; SendToDock converts only when a port is open.
  SendToDock (app, title, artist, album, status);

  // Position / duration - handy later for a progress bar on the LCD dock.
  // Timeline is optional: some sources (radio streams...) don't provide one,
  // and the API throws in that case. Hence the try/catch, not a hard failure.
  try
    {
      auto timeline = session.GetTimelineProperties ();
      auto posSec   = std::chrono::duration_cast<std::chrono::seconds> (timeline.Position ()).count ();
      auto endSec   = std::chrono::duration_cast<std::chrono::seconds> (timeline.EndTime ()).count ();
      std::wcout << L"  Position        : " << posSec << L"s / " << endSec << L"s\n";
    }
  catch (...)
    { /* some sources don't provide a timeline */
    }

  if (saveThumbnail)
    {
      SaveThumbnail (properties.Thumbnail (), L"thumbnail");
    }
}

// Same as DescribeSession, but catches: session APIs throw hresult_error when
// the source app vanished mid-query. The rule: an event callback must never
// let an exception escape — WinRT would terminate the process. Catch-all too:
// non-WinRT exceptions (bad_alloc...) are outside hresult_error.
static void DescribeSessionSafe (GlobalSystemMediaTransportControlsSession const &session, bool saveThumbnail)
{
  try
    {
      DescribeSession (session, saveThumbnail);
    }
  catch (hresult_error const &e)
    {
      std::wcout << L"  <error: " << e.message () << L">\n";
    }
  catch (...)
    {
      std::wcout << L"  <error: unknown exception while describing session>\n";
    }
}

// -------------------------------------------------------------------------------------
// Keeping sessions alive + auto-detaching their events.
//
// WHY A WATCHER STRUCT? WinRT events are "fire and forget" unless you hold
// something. Subscribing with auto_revoke returns an event_revoker; when that
// revoker is destroyed (or .revoke() is called), the subscription is removed.
// We therefore keep, per session:
//   - the session object itself (strong ref: keeps it alive / queryable)
//   - the two revokers (destroying them unsubscribes cleanly)
// Storing all of this in a vector means "s_watchers.clear()" is the ONE
// statement that tears down every subscription — no manual loop needed.
// -------------------------------------------------------------------------------------
using SessionEventRevoker = winrt::event_revoker<Windows::Media::Control::IGlobalSystemMediaTransportControlsSession>;
struct SessionWatcher
{
  // NB: SMTC classes are not default-constructible in modern SDKs, so the
  // member must be explicitly initialized with nullptr.
  GlobalSystemMediaTransportControlsSession session{ nullptr };
  SessionEventRevoker                       mediaRevoker;    // fires on track change
  SessionEventRevoker                       playbackRevoker; // fires on play/pause/etc.
};
// NB: cleared/rebuilt from the SessionsChanged handler, which runs on a
// threadpool thread. SMTC coalesces manager events today, so concurrent
// mutations are not expected; add a mutex if that ever changes.
static std::vector<SessionWatcher> s_watchers;

// Re-enumerates all sessions, prints them and (re)attaches per-session events.
// Called once at startup and again every time the set of players changes
// (Spotify launched, Edge closed, ...). The clear() at the top detaches all
// old subscriptions, so re-calling never double-subscribes.
static void AttachSessionWatchers (GlobalSystemMediaTransportControlsSessionManager const &manager)
{
  s_watchers.clear (); // revokes every per-session subscription (see above)

  auto sessions = manager.GetSessions ();
  std::wcout << Timestamp () << L"Media sessions: " << sessions.Size () << L"\n";

  // Both per-session events need the same reaction: print a banner, then
  // re-describe. This little factory avoids writing the lambda twice; the
  // outer lambda is a factory, the inner one is the actual handler.
  auto describeOnChange = [] (wchar_t const *banner, bool saveThumbnail)
    {
      return [ banner, saveThumbnail ] (GlobalSystemMediaTransportControlsSession const &sender, auto &&)
        {
          std::wcout << Timestamp () << banner << L":\n";
          DescribeSessionSafe (sender, saveThumbnail); // never throws out of a handler
        };
    };

  for (auto &&session : sessions)
    {
      std::wcout << L"  ----------------------------------------\n";
      DescribeSessionSafe (session, true);

      SessionWatcher watcher;
      watcher.session      = session; // hold a strong ref so events keep firing
      watcher.mediaRevoker = session.MediaPropertiesChanged (auto_revoke, describeOnChange (L"Now playing changed", true));
      watcher.playbackRevoker
          = session.PlaybackInfoChanged (auto_revoke, describeOnChange (L"Playback info changed", false));
      s_watchers.push_back (std::move (watcher));
    }
}

// =====================================================================================
// SECTION 5: main — wiring it all together
// =====================================================================================

int main (int argc, char **argv)
{
  // --oneshot: enumerate once, print, exit. Used by automated tests/scripts
  // (no interactive "press Enter" wait at the end).
  bool const oneShot = (argc > 1 && std::string (argv[1]) == "--oneshot");

  // ---- Console setup -------------------------------------------------------
  // Two calls, both needed:
  //   _setmode(_O_U8TEXT): CRT-level: makes wcout emit UTF-8.
  //   SetConsoleOutputCP(CP_UTF8): console-level: makes the terminal
  //                                INTERPRET those bytes as UTF-8.
  // Without them, non-ASCII titles (emoji, CJK) print as garbage.
  _setmode (_fileno (stdout), _O_U8TEXT);
  SetConsoleOutputCP (CP_UTF8);

  // ---- COM apartment -------------------------------------------------------
  // Every COM/WinRT thread lives in an "apartment". multi_threaded (MTA) means
  // "no windows here, calls can run on any thread" — exactly what we want,
  // because SMTC callbacks arrive on threadpool threads and our .get() calls
  // need to complete on the threadpool. (An STA would deadlock them.)
  init_apartment (apartment_type::multi_threaded);

  std::wcout << L"=== MediaDock companion app ===\n\n";

  std::wcout << L"Audio devices:\n";
  ListSpeakers ();
  std::wcout << L"\n";

  // ---- Open the dock link (optional!) --------------------------------------
  // openSerialPort() throws std::runtime_error if COM4 doesn't exist. No dock
  // attached is a NORMAL situation, so we catch and continue: everything else
  // still works, SendToDock just no-ops (see the !g_comPort check there).
  try
    {
      g_comPort = winrt::handle{ openSerialPort (L"COM4") };
    }
  catch (std::exception const &e)
    {
      std::wcout << L"Serial port not available (" << e.what () << L"), running without dock output\n";
    }

  // ---- Get the media hub ----------------------------------------------------
  // RequestAsync is the documented way to obtain the session manager (there
  // is no plain constructor). .get() blocks until the OS hands it to us.
  //
  // Every app that integrates with the Windows media flyout (Spotify, Edge,
  // Chrome, Windows Media Player, ...) shows up here as a session.
  auto manager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync ().get ();

  // ---- Attach per-session watchers ------------------------------------------
  // Enumerate current players, print them, subscribe to their change events.
  try
    {
      AttachSessionWatchers (manager);
    }
  catch (hresult_error const &e)
    {
      std::wcout << L"  <error: " << e.message () << L">\n";
    }

  if (!oneShot)
    {
      std::wcout << L"\nListening for changes... (press Enter to quit)\n\n";

      // ---- Manager-level events (live until process exit) -------------------
      // SessionsChanged: a player appeared or went away. Rebuild the whole
      // watcher list (the clear() inside detaches the stale subscriptions).
      manager.SessionsChanged (
          [] (GlobalSystemMediaTransportControlsSessionManager const &sender, auto &&)
            {
              try
                {
                  AttachSessionWatchers (sender);
                }
              catch (hresult_error const &e)
                {
                  std::wcout << L"  <error: " << e.message () << L">\n";
                }
            });

      // CurrentSessionChanged: the "featured" session switched, e.g. you
      // started a video while music was playing. Report the new focus.
      manager.CurrentSessionChanged (
          [] (GlobalSystemMediaTransportControlsSessionManager const &sender, auto &&)
            {
              try
                {
                  if (auto current = sender.GetCurrentSession ())
                    {
                      std::wcout << Timestamp () << L"Current session changed:\n";
                      DescribeSessionSafe (current, false);
                    }
                }
              catch (hresult_error const &e)
                {
                  std::wcout << L"  <error: " << e.message () << L">\n";
                }
            });

      // ---- Keep main() alive ---------------------------------------------
      // Everything above is EVENT-DRIVEN: the handlers run on OS threadpool
      // threads, not on this thread. main() would exit immediately without
      // this blocking read, killing all subscriptions with it.
      std::wstring dummy;
      std::getline (std::wcin, dummy);
    }

  // ---- Teardown -------------------------------------------------------------
  // Close under the mutex: an SMTC callback may still be INSIDE SendToDock
  // (running on a threadpool thread) while we exit. The handle must not be
  // freed under it — hence the same mutex that SendToDock holds.
  // (winrt::handle would also close itself at process exit; this explicit
  // close just makes the ordering deterministic.)
  std::scoped_lock const portLock (g_serialMutex);
  g_comPort.close ();
  return 0;
}
