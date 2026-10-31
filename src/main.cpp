// MediaDock companion app (proof-of-concept)
//
// Captures from Windows:
//   1. WHAT is playing  -> title / artist / album        (System Media Transport Controls, SMTC)
//   2. FROM WHERE       -> source app (Spotify, Edge...) (SMTC session SourceAppUserModelId)
//   3. ON WHAT SPEAKER  -> default render device name    (MMDevice API / Core Audio)
//   4. THUMBNAIL        -> album art as RGB565 pixels   (SMTC session Thumbnail)
//
// and sends the media metadata to the LCD dock over COM4 using the TLV/COBS
// wire protocol defined in inc/serializer.hpp (CRC-16 protected; decoder
// docs in info/protocol.md):
//   - metadata frame on every media event: app / title / artist / album /
//     playbackStatus (tags 1-5) + position (tag 6)
//   - art frame on art change: album art is scaled to fit 160x160
//     (aspect preserved - 16:9 art comes out 160x88) and packed raw
//     RGB565 (WIC), sent as its own raw frame (0x00 0x00 kind marker +
//     w/h header + pixels, no COBS/TLV/CRC) 50 ms after the data frame,
//     only when its pixels actually changed
//   - 1 Hz ticker: position-only frame (tag 6) so the dock can interpolate
//     the progress bar between real events
//
// APIs used (both ship inside the Windows SDK, nothing extra to install):
//   - C++/WinRT projection headers  <winrt/Windows.Media.Control.h>
//   - COM: IMMDeviceEnumerator (mmdeviceapi.h) for audio endpoint names
//
// NOTE on the API surface: SDKs from Windows 11 24H2 (build 26100) onward
// expose the modernized SMTC API used here:
//   - GlobalSystemMediaTransportControlsSessionManager::GetSessions()  (sync)
//   - GlobalSystemMediaTransportControlsSession::GetPlaybackInfo()     (sync)
//   - GlobalSystemMediaTransportControlsSession::GetTimelineProperties() (sync)
//   - per-session events (MediaPropertiesChanged / PlaybackInfoChanged)
// If you ever need to support older Windows 10 builds, retarget an older SDK
// (e.g. 10.0.22621.0) and switch back to the *Async variants.
//
// Usage:
//   WinApp.exe                -> list everything once, then keep printing live
//                                media changes until you press Enter
//   WinApp.exe --oneshot      -> list everything once and exit (for automated testing)
//   WinApp.exe --no-thumbnail -> skip album art entirely (no fetch, no scale,
//                                no multi-second art write to the dock)
//   Flags are order-free and combinable: WinApp.exe --oneshot --no-thumbnail

#include <windows.h>
#include <conio.h>
// WIC (Windows Imaging Component): album art decode + downscale + RGB565
// pack. Must come after windows.h (needs the OLE headers it pulls in).
#include <wincodec.h>

// C++/WinRT projection headers (from the Windows SDK).
// Windows.ApplicationModel.h must come before Windows.Management.Deployment.h
// so Package::Id() is fully defined when we call it.
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Management.Deployment.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>

// Core Audio / MMDevice API (also part of the Windows SDK)
// NOTE: mmdeviceapi.h must come first: it (via propsys.h -> propkeydef.h)
// defines the DEFINE_PROPERTYKEY macro that functiondiscoverykeys_devpkey.h
// only *uses* (its own devpropdef.h include is commented out in the SDK).
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>

#include <fcntl.h>
#include <io.h>

#include <chrono>
#include <cstdint>
#include <format>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "COMPort.hpp"
#include "serializer.hpp"

#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

using namespace winrt;
using namespace Windows::Media::Control;

// ---------------------------------------------------------------------------
// Serial link ("to the dock")
// ---------------------------------------------------------------------------

static winrt::handle g_comPort; // RAII: closed automatically, even on early return
static serializer    g_serializer;

// Last thumbnail frame that was fully written to the dock (guarded by
// g_serialMutex): art is only re-sent when its bytes actually change.
static std::vector<uint8_t> g_lastThumbnailSent;

// Album art master switch (--no-thumbnail): false skips the whole art path
// (fetch, scale, multi-second serial write). Written once in main() before
// any session watcher exists, read-only after that - no locking needed.
static bool g_sendThumbnails = true;

// "Now playing" hub, kept as a global so the 1 Hz position ticker can query
// the current session without threading the manager through every callback.
static GlobalSystemMediaTransportControlsSessionManager g_manager{ nullptr };
// Signature of the metadata last pushed to the dock. SMTC change events are
// not reliable across apps/sessions (a track change inside the browser does
// not always raise MediaPropertiesChanged), so the 1 Hz ticker also compares
// this signature and resends on any difference.
static std::mutex  g_signatureMutex;
static std::wstring g_lastSignature;
// Art delivery bookkeeping: SMTC may not have the new thumbnail ready at the
// instant the track changes, so the ticker retries a few times instead of
// leaving the dock on the previous cover.
static std::wstring g_artSignature;
static std::wstring g_artAttemptSignature;
static int          g_artAttempts = 0;


// SMTC events arrive on threadpool threads, so several callbacks can try to
// send at once. The serializer and the port write are not safe to interleave
// (frames would corrupt); one mutex around the whole send keeps frames whole.
static std::mutex g_serialMutex;
// Dock link state. The dock re-enumerates on every reset/flash and may be
// plugged in after this app starts, so the port is opened lazily and reopened
// whenever a write fails. All of this is guarded by g_serialMutex.
static bool                                g_dockConnected = false;
static std::wstring                        g_dockPortName;
static int                                  g_writeFailures = 0; // consecutive
static std::chrono::steady_clock::time_point g_nextConnectAttempt{};

// Short text for the Win32 errors WriteFile can produce on this link, so a
// failure log line is attributable without a lookup table.
static const wchar_t *WinErrorName (DWORD err)
{
  switch (err)
    {
    case ERROR_SEM_TIMEOUT: // 121
      return L"ERROR_SEM_TIMEOUT: write deadline expired, device stopped accepting bytes (dock-side RX stall)";
    case ERROR_INVALID_HANDLE: // 6
      return L"ERROR_INVALID_HANDLE: port handle is gone";
    case ERROR_ACCESS_DENIED: // 5
      return L"ERROR_ACCESS_DENIED: port held by another process";
    case ERROR_OPERATION_ABORTED: // 995
      return L"ERROR_OPERATION_ABORTED: write aborted by a comm error or close";
    case ERROR_DEVICE_NOT_CONNECTED: // 1167
      return L"ERROR_DEVICE_NOT_CONNECTED: dock unplugged";
    default:
      return L"unmapped Win32 error";
    }
}

static std::wstring Timestamp (); // defined below, used by the link logging

// Opens the dock port if it is not connected yet. Assumes g_serialMutex is
// held. Retries are throttled so a missing dock cannot spam CreateFile/logs.
// On success the metadata signature is invalidated, which makes the ticker
// push the current track (and art) to the freshly attached dock.
static void EnsureDockConnectedLocked ()
{
  if (g_dockConnected && g_comPort)
    {
      return;
    }
  if (!g_dockConnected && g_comPort)
    {
      // Half-open state (handle alive, link marked down): drop the handle so
      // this process cannot end up fighting itself for the COM port, which
      // shows up as "Access is denied" on the retry and never clears.
      g_comPort.close ();
    }

  const auto now = std::chrono::steady_clock::now ();
  if (now < g_nextConnectAttempt)
    {
      return;
    }
  g_nextConnectAttempt = now + std::chrono::seconds (2);

  std::wstring portName = findDockPort ();
  if (portName.empty ())
    {
      wchar_t envPort[64]{};
      const DWORD envLen = GetEnvironmentVariableW (L"MEDIADOCK_COM_PORT", envPort, static_cast<DWORD> (std::size (envPort)));
      portName = ((envLen > 0U) && (envLen < std::size (envPort))) ? std::wstring (envPort) : std::wstring (L"COM4");
    }

  try
    {
      g_comPort = winrt::handle{ openSerialPort (portName) };
      g_dockConnected = true;
      g_dockPortName = portName;
      {
        std::scoped_lock const sigLock (g_signatureMutex);
        g_lastSignature.clear (); // force a metadata + art resend
      }
      std::wcout << Timestamp () << L"Dock connected on " << portName << L"\n";
    }
  catch (std::exception const &e)
    {
      g_comPort.close ();
      g_dockConnected = false;
      // Retries run every couple of seconds; log them at most every 10 s so a
      // dock that stays absent does not flood the console.
      static std::chrono::steady_clock::time_point lastWarn{};
      const auto                                    now2 = std::chrono::steady_clock::now ();
      if ((lastWarn.time_since_epoch ().count () == 0) || ((now2 - lastWarn) >= std::chrono::seconds (10)))
        {
          lastWarn = now2;
          std::wcout << Timestamp () << L"Dock not available on " << portName << L" (" << e.what () << L"), retrying\n";
        }
    }
  std::wcout.flush ();
}

// Closes the port after a failed write. Assumes g_serialMutex is held.
static void MarkDockLostLocked (const wchar_t *reason)
{
  if (!g_dockConnected)
    {
      return;
    }
  g_comPort.close ();
  g_dockConnected = false;
  g_nextConnectAttempt = std::chrono::steady_clock::now () + std::chrono::seconds (1);
  std::wcout << Timestamp () << L"Dock disconnected (" << reason << L"), will reconnect\n";
}

// Writes the current serializer buffer to the port. Assumes g_serialMutex is
// held and g_comPort is open. frameKind ("metadata" / "position" /
// "thumbnail") exists purely so failure logs show which traffic is hit.
// logSuccess=false for the 1 Hz position frames: a per-second success line
// would drown out everything else on the console.
// Returns true only on a complete write.
static bool WriteFrameLocked (size_t frameSize, std::wstring_view frameKind, bool logSuccess = true)
{
  // COMSTAT.cbOutQue is the evidence that attributes write failures: it is
  // the byte count still queued host-side. Consecutive failures with a queue
  // that stays full (or grows) mean the host is handing bytes to the driver
  // fine while the device drains nothing -> far-end (dock firmware) stall.
  DWORD   commErr = 0;
  COMSTAT before{};
  ClearCommError (g_comPort.get (), &commErr, &before); // surface + clear any stale driver error state
  if (commErr)
    {
      std::wcout << L"serial: comm error mask 0x" << std::hex << commErr << std::dec << L"\n";
    }

  DWORD      written = 0;
  const BOOL ok
      = WriteFile (g_comPort.get (), g_serializer.buffer ().data (), static_cast<DWORD> (frameSize), &written, nullptr);
  // Capture the error immediately: stream output below goes through the CRT
  // and is free to overwrite the thread's last-error code before we read it.
  const DWORD err = ok ? 0 : GetLastError ();

  if (ok && static_cast<size_t> (written) == frameSize)
    {
      g_writeFailures = 0;
      if (logSuccess)
        {
          std::wcout << L"serial: sent frame, " << written << L" bytes\n";
        }
      return true;
    }

  // Queue depth after the attempt, for the attribution evidence above.
  DWORD   afterErr = 0;
  COMSTAT after{};
  ClearCommError (g_comPort.get (), &afterErr, &after);

  if (!ok)
    {
      std::wcout << L"serial: write failed, err=" << err << L" (" << WinErrorName (err) << L"), frame=" << frameKind
                 << L", " << frameSize << L" bytes, host tx queue " << before.cbOutQue << L" -> " << after.cbOutQue
                 << L" bytes\n";
    }
  else
    {
      // The port has write timeouts, so WriteFile can return TRUE having sent
      // only part of the frame; the dock then resyncs on the next 0x00.
      std::wcout << L"serial: partial write, " << written << L"/" << frameSize << L" bytes, frame=" << frameKind
                 << L", host tx queue " << before.cbOutQue << L" -> " << after.cbOutQue << L" bytes\n";
    }
  // One timeout is often transient (the dock is busy rendering a cover). Keep
  // the handle and retry next tick; the port is only dropped after a sustained
  // run of failures, because closing it makes usbser.sys re-open the device and
  // a dock that suspends mid-open needs a re-enumeration to come back.
  if (++g_writeFailures < 5)
    {
      return false;
    }
  MarkDockLostLocked (ok ? L"partial write" : L"write failed");
  return false;
}

// Builds one TLV/COBS frame from the current media metadata and writes it to
// the dock. Safe to call from any thread. Silently does nothing when the
// serial port is not open (e.g. dock unplugged), so media watching keeps
// working without the hardware attached.
static void SendToDock (std::wstring_view app, std::wstring_view title, std::wstring_view artist,
                        std::wstring_view album, std::string_view playbackStatus, uint32_t positionMs,
                        uint32_t durationMs)
{
  std::scoped_lock const lock (g_serialMutex);

  if (!g_comPort)
    {
      return;
    }

  // Convert to the wire's UTF-8 format only now, after the port check above:
  // headless runs (dock unplugged) pay nothing per media event.
  g_serializer.setApp (winrt::to_string (app));
  g_serializer.setTitle (winrt::to_string (title));
  g_serializer.setArtist (winrt::to_string (artist));
  g_serializer.setAlbum (winrt::to_string (album));
  g_serializer.setPlaybackStatus (playbackStatus);
  g_serializer.setPosition (positionMs, durationMs);

  WriteFrameLocked (g_serializer.encode (), L"metadata");
}

// 1 Hz ticker path: a tiny frame carrying only the position field (tag 6).
// The dock interpolates between these; TLV frames tolerate missing tags, so
// no full metadata resend is needed. All serializer access under the mutex.
static void SendPositionToDock (uint32_t positionMs, uint32_t durationMs)
{
  std::scoped_lock const lock (g_serialMutex);

  EnsureDockConnectedLocked ();
  if (!g_comPort)
    {
      return;
    }

  g_serializer.setPosition (positionMs, durationMs);
  const size_t frameSize = g_serializer.encodePositionFrame ();
  if (WriteFrameLocked (frameSize, L"position", false))
    {
      // Success evidence for the 1 Hz frames: value + COBS block hex in the
      // exact format the dock's writeback prints it (frame minus preamble,
      // 3-byte length and delimiter), so sender log and dock log can be
      // diffed byte for byte. One line per second is the point: repeated
      // identical lines mean the position itself is not advancing.
      std::wcout << L"serial: sent position " << positionMs << L"/" << durationMs << L" ms, cobs=";
      const std::vector<uint8_t> &buf = g_serializer.buffer ();
      for (size_t i = 5; i + 1 < buf.size (); ++i) // skip 55 AA + len:3, drop the trailing delimiter
        {
          std::wcout << std::format (L"{:02x}", buf[i]);
        }
      std::wcout << L"\n";
    }
}

// Art path: pixels go out as their own raw art frame (0x00 0x00 kind
// marker + w/h + raw RGB565, no COBS/TLV/CRC - see serializer.hpp), and
// only when they differ from the last frame that was fully written.

// The art frame floods 28-51 KB down the link (~2.5-4.5 s at 115200).
// The 50 ms gap after the preceding data frame gives the dock a beat to
// finish parsing the small frame before the pixels start arriving.
constexpr auto kArtFrameGap = std::chrono::milliseconds (50);

static void SendThumbnailToDock (const std::vector<uint8_t> &rgb565, uint16_t width, uint16_t height, bool force = false)
{
  std::scoped_lock const lock (g_serialMutex);

  EnsureDockConnectedLocked ();
  if (!g_comPort || rgb565.empty ())
    {
      return;
    }

  {
    // Same bytes are common (same album/thumbnail), but the dock cleared its
    // cover for the new track, so a repeat track must still resend the art.
    std::scoped_lock const sigLock (g_signatureMutex);
    if (!force && (rgb565 == g_lastThumbnailSent) && (g_artSignature == g_lastSignature))
      {
        return;
      }
  }

  g_serializer.setThumbnail (rgb565, width, height);
  const size_t frameSize = g_serializer.encodeThumbnailFrame ();
  if (!g_serializer.bufferValid ())
    {
      return;
    }

  // Console evidence for firmware bring-up: CRC-16/CCITT-FALSE over the
  // art body ([width:2 LE][height:2 LE][raw pixels] - exactly the bytes
  // len:3 counts), so the dock can recompute the same value over what it
  // received and diff it against this line. The body sits between the
  // 7-byte head (2 preamble + 3 length + 2 kind marker) and the trailing
  // delimiter.
  const std::vector<uint8_t> &frame = g_serializer.buffer ();
  const uint16_t bodyCrc = serializer::crc16 (frame.data () + 7, frame.size () - 8);
  std::wcout << std::format (L"  Thumbnail       : CRC-16 (w:2 LE, h:2 LE, pixels) = 0x{:04X}\n", bodyCrc);

  // Tiny gap between the data frame and the art flood (see kArtFrameGap).
  // Taken under the mutex so no other frame wedges itself in between.
  std::this_thread::sleep_for (kArtFrameGap);

  // Remember only on a complete write: after a partial write the dock has a
  // torn frame and will re-hunt the preamble; keeping the old copy means
  // the next change event retries the new art instead of skipping it.
  if (WriteFrameLocked (frameSize, L"thumbnail"))
    {
      g_lastThumbnailSent = rgb565;
      std::scoped_lock const sigLock (g_signatureMutex);
      g_artSignature = g_lastSignature;
    }
}

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

static std::wstring Timestamp ()
{
  return std::format (L"[{:%H:%M:%S}] ",
                      std::chrono::zoned_time{ std::chrono::current_zone (), std::chrono::system_clock::now () });
}

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

// Position / duration for the wire, in ms (0xFFFFFFFF = unknown).
//
// SMTC timeline Position is an ANCHOR, not a running clock: source apps
// usually set it once per track change / seek and never again, so sampling
// it at 1 Hz puts the SAME bytes on the wire every second (seen on the dock's
// writeback: seven identical frames while playing). The dock re-anchors its
// progress bar on every frame, so a frozen anchor freezes the bar. While the
// session is Playing, advance the anchor by the wall time since the app last
// touched the timeline (LastUpdatedTime). Paused / stopped report the raw
// anchor: the wall delta would overshoot the pause point.
static std::pair<uint32_t, uint32_t> WirePosition (GlobalSystemMediaTransportControlsSession const &session)
{
  uint32_t positionMs = 0xFFFFFFFFU;
  uint32_t durationMs = 0xFFFFFFFFU;
  try
    {
      auto            timeline = session.GetTimelineProperties ();
      long long       pos = std::chrono::duration_cast<std::chrono::milliseconds> (timeline.Position ()).count ();
      const long long end = std::chrono::duration_cast<std::chrono::milliseconds> (timeline.EndTime ()).count ();

      if (pos >= 0 && session.GetPlaybackInfo ().PlaybackStatus ()
                        == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing)
        {
          // LastUpdatedTime is 100 ns ticks since 1601; a default value means
          // the app never set one - then the raw anchor is all we have.
          const auto updated = timeline.LastUpdatedTime ();
          const auto now = winrt::clock::now ();
          if (updated.time_since_epoch ().count () > 0 && now > updated)
            {
              pos += std::chrono::duration_cast<std::chrono::milliseconds> (now - updated).count ();
            }
        }
      if (end >= 0 && pos > end)
        {
          pos = end; // runaway extrapolation or bogus anchor: never past the track
        }
      if (pos >= 0)
        {
          positionMs = static_cast<uint32_t> (pos);
        }
      if (end >= 0)
        {
          durationMs = static_cast<uint32_t> (end);
        }
    }
  catch (...)
    { /* some sources don't provide a timeline */
    }
  return { positionMs, durationMs };
}

// Resolve a SourceAppUserModelId into a friendly app name.
//  - Packaged apps look like "Microsoft.ZuneMusic_8wekyb3d8bbwe!Microsoft.ZuneMusic"
//    -> we look the package family up via PackageManager and use its name.
//  - Win32 apps usually report something like "chrome.exe" or "Spotify"
//    -> use it as-is.
static std::wstring FriendlyAppName (std::wstring const &aumid)
{
  if (auto bang = aumid.find (L'!'); bang != std::wstring::npos)
    {
      std::wstring family = aumid.substr (0, bang);
      try
        {
          Windows::Management::Deployment::PackageManager pm;
          auto pkg = pm.FindPackageForUser (L"", family); // L"" = current user
          if (pkg)
            {
              return std::wstring{ pkg.Id ().Name () };
            }
        }
      catch (...)
        { /* fall through */
        }
      return family;
    }

  // Unpackaged apps (browsers, portable players) report a bare AUMID such as
  // "Chromium.EHRFVF7QOKQDASYIPZ2IBCUVVI". The trailing block is a hash, not
  // something a user recognises, so keep the leading product name when the
  // identifier looks like "<Name>.<HASH>" or "<Name>.exe".
  const auto dot = aumid.find (L'.');
  if ((dot != std::wstring::npos) && (dot > 0U))
    {
      std::wstring head = aumid.substr (0U, dot);
      if (_wcsicmp (head.c_str (), L"chromium") == 0)
        {
          return L"Chromium"; // Brave/Chrome/Edge videos arrive under this
        }
      return head;
    }
  return aumid;
}

// ---------------------------------------------------------------------------
// Audio endpoints ("on what speaker")
// ---------------------------------------------------------------------------

static std::wstring GetDeviceFriendlyName (IMMDevice *device)
{
  std::wstring                   name = L"<unknown>";
  winrt::com_ptr<IPropertyStore> store;
  if (SUCCEEDED (device->OpenPropertyStore (STGM_READ, store.put ())))
    {
      PROPVARIANT var;
      PropVariantInit (&var);
      if (SUCCEEDED (store->GetValue (PKEY_Device_FriendlyName, &var)) && var.vt == VT_LPWSTR && var.pwszVal)
        {
          name = var.pwszVal;
        }
      PropVariantClear (&var);
    }
  return name;
}

// Lists the default render device plus every active render device (speakers,
// headphones, HDMI, Bluetooth, ...).
static void ListSpeakers ()
{
  winrt::com_ptr<IMMDeviceEnumerator> enumerator;
  winrt::check_hresult (CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                          __uuidof (IMMDeviceEnumerator), enumerator.put_void ()));

  // The device new audio will play on by default:
  winrt::com_ptr<IMMDevice> defaultDevice;
  if (SUCCEEDED (enumerator->GetDefaultAudioEndpoint (eRender, eMultimedia, defaultDevice.put ())))
    {
      std::wcout << L"  Default speaker : " << GetDeviceFriendlyName (defaultDevice.get ()) << L"\n";
    }

  // Every render endpoint that is currently active:
  winrt::com_ptr<IMMDeviceCollection> collection;
  if (FAILED (enumerator->EnumAudioEndpoints (eRender, DEVICE_STATE_ACTIVE, collection.put ())))
    {
      return;
    }

  UINT count = 0;
  collection->GetCount (&count);
  for (UINT i = 0; i < count; ++i)
    {
      winrt::com_ptr<IMMDevice> device;
      if (FAILED (collection->Item (i, device.put ())))
        {
          continue;
        }
      std::wcout << L"  Active endpoint : " << GetDeviceFriendlyName (device.get ()) << L"\n";
    }
}

// ---------------------------------------------------------------------------
// Media sessions ("what is playing", "from where", thumbnail)
// ---------------------------------------------------------------------------

// Downscale album art for the dock and pack it as raw RGB565 (5-6-5,
// BIG-ENDIAN per pixel: the byte order a bare SPI panel shifts out, so the
// MCU can DMA the buffer straight to the display - no byte swapping, no
// image decoder). Longest side -> 160 px, aspect preserved: 16:9 art
// comes out 160x88, portrait art e.g. 88x160 (the dock centers
// non-square results in its art box). A 160x160 result is 51 200 pixel
// bytes: ~4.5 s of transfer at 115200 baud. Returns an empty vector on
// any failure - a missing thumbnail is cosmetic, never fatal. On success
// outWidth/outHeight receive the scaled size for the art frame header.
static std::vector<uint8_t> ScaleThumbnailRgb565 (const std::vector<uint8_t> &image, uint16_t &outWidth,
                                                  uint16_t &outHeight)
{
  std::vector<uint8_t> out;
  outWidth = 0;
  outHeight = 0;

  winrt::com_ptr<IWICImagingFactory> factory;
  if (FAILED (CoCreateInstance (CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory,
                                factory.put_void ())))
    {
      return out;
    }

  // The WIC factory has no CreateDecoderFromMemory: wrap the bytes in an
  // IStream instead. The stream owns (frees) the HGLOBAL on release.
  HGLOBAL hMem = GlobalAlloc (GMEM_MOVEABLE, image.size ());
  if (!hMem)
    {
      return out;
    }
  void *memBytes = GlobalLock (hMem);
  if (!memBytes)
    {
      GlobalFree (hMem);
      return out;
    }
  memcpy (memBytes, image.data (), image.size ());
  GlobalUnlock (hMem);

  winrt::com_ptr<IStream> sourceStream;
  if (FAILED (CreateStreamOnHGlobal (hMem, TRUE, sourceStream.put ())))
    {
      GlobalFree (hMem); // only on failure: on success the stream owns it
      return out;
    }

  winrt::com_ptr<IWICBitmapDecoder> decoder;
  if (FAILED (factory->CreateDecoderFromStream (sourceStream.get (), nullptr, WICDecodeMetadataCacheOnDemand,
                                                decoder.put ())))
    {
      return out;
    }

  winrt::com_ptr<IWICBitmapFrameDecode> sourceFrame;
  if (FAILED (decoder->GetFrame (0, sourceFrame.put ())))
    {
      return out;
    }

  UINT width = 0;
  UINT height = 0;
  sourceFrame->GetSize (&width, &height);
  if (width == 0 || height == 0)
    {
      return out;
    }

  // Fit within 160x160 (longest side -> 160 px): 160x160 RGB565 =
  // 51 200 bytes, ~4.5 s on the wire.
  constexpr UINT kLongSide = 160;
  UINT           thumbW = 0;
  UINT           thumbH = 0;
  if (width >= height)
    {
      thumbW = kLongSide;
      thumbH = static_cast<UINT> (static_cast<uint64_t> (height) * kLongSide / width);
    }
  else
    {
      thumbH = kLongSide;
      thumbW = static_cast<UINT> (static_cast<uint64_t> (width) * kLongSide / height);
    }
  if (thumbW == 0 || thumbH == 0)
    {
      return out; // degenerate aspect (e.g. 1-px-wide source)
    }

  // High-quality downscale (Fant = area average), converted to 24bpp BGR -
  // a simple byte layout to pack from ([B][G][R] per pixel).
  winrt::com_ptr<IWICBitmapScaler> scaler;
  if (FAILED (factory->CreateBitmapScaler (scaler.put ()))
      || FAILED (scaler->Initialize (sourceFrame.get (), thumbW, thumbH, WICBitmapInterpolationModeFant)))
    {
      return out;
    }

  winrt::com_ptr<IWICFormatConverter> converter;
  if (FAILED (factory->CreateFormatConverter (converter.put ()))
      || FAILED (converter->Initialize (scaler.get (), GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, nullptr,
                                        0.0, WICBitmapPaletteTypeCustom)))
    {
      return out;
    }

  // Copy the scaled pixels and pack RGB565 big-endian: red's 5 bits land
  // in the first wire byte (MSB first), exactly the order a bare SPI panel
  // shifts them out.
  std::vector<uint8_t> pixels (static_cast<size_t> (thumbW) * thumbH * 3);
  if (FAILED (converter->CopyPixels (nullptr, thumbW * 3, static_cast<UINT> (pixels.size ()), pixels.data ())))
    {
      return out;
    }

  // Pack tightly at the scaled size (no padding): raw RGB565 big-endian,
  // red's 5 bits in the first wire byte (MSB first), exactly the order a
  // bare SPI panel shifts them out.
  out.reserve (static_cast<size_t> (thumbW) * thumbH * 2);
  for (size_t i = 0; i < pixels.size (); i += 3)
    {
      const uint8_t b = pixels[i];
      const uint8_t g = pixels[i + 1];
      const uint8_t r = pixels[i + 2];
      const auto    pixel = static_cast<uint16_t> (((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
      out.push_back (static_cast<uint8_t> (pixel >> 8)); // MSB first: direct panel order
      out.push_back (static_cast<uint8_t> (pixel & 0xFF));
    }

  outWidth = static_cast<uint16_t> (thumbW);
  outHeight = static_cast<uint16_t> (thumbH);
  return out;
}

// Reads the album art bytes off the session's thumbnail reference, scales
// them and forwards the result to the dock (which dedupes on unchanged art).
static void SendThumbnailIfChanged (Windows::Storage::Streams::IRandomAccessStreamReference const &thumbRef, bool force = false)
{
  if (!thumbRef)
    {
      return;
    }

  try
    {
      // .get() blocks a threadpool thread; safe only because this app runs an
      // MTA apartment and the async completes on the threadpool.
      auto stream = thumbRef.OpenReadAsync ().get ();
      if (stream.Size () == 0)
        {
          return;
        }

      auto const                            byteCount = static_cast<uint32_t> (stream.Size ());
      Windows::Storage::Streams::DataReader reader (stream);
      reader.LoadAsync (byteCount).get ();
      winrt::com_array<uint8_t> bytes (byteCount);
      reader.ReadBytes (bytes);

      std::wcout << L"  Thumbnail       : " << bytes.size () << L" bytes -> ";
      uint16_t                   thumbW = 0;
      uint16_t                   thumbH = 0;
      std::vector<uint8_t> const scaled = ScaleThumbnailRgb565 ({ bytes.begin (), bytes.end () }, thumbW, thumbH);
      if (scaled.empty ())
        {
          std::wcout << L"scale failed, skipped\n";
          return;
        }
      std::wcout << scaled.size () << L" byte RGB565 (" << thumbW << L"x" << thumbH << L")\n";

      SendThumbnailToDock (scaled, thumbW, thumbH, force);
    }
  catch (...)
    {
      // A vanished stream or hostile app must not kill the event handler.
      std::wcout << L"  Thumbnail       : <unavailable>\n";
    }
}

// Prints everything we know about one media session and forwards it to the
// dock. saveThumbnail only matters on the initial enumeration; on live events
// the thumbnail never changes, so re-saving it would just spam the disk.
// Pick the session the dock should mirror. Windows' "current" session can be a
// stale/paused app (e.g. Spotify left paused) while another app is playing, so
// a Playing session always wins; otherwise fall back to the current session,
// then to any session.
static GlobalSystemMediaTransportControlsSession SelectBestSession (
    GlobalSystemMediaTransportControlsSessionManager const &manager)
{
  GlobalSystemMediaTransportControlsSession fallback{ nullptr };
  for (auto &&session : manager.GetSessions ())
    {
      if (!fallback)
        {
          fallback = session;
        }
      // A dead session (app closed while Windows keeps the entry around) can
      // throw here; it must not abort the scan or the caller's tick.
      try
        {
          if (session.GetPlaybackInfo ().PlaybackStatus ()
              == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing)
            {
              return session;
            }
        }
      catch (...)
        { /* treat an unqueryable session as not playing */
        }
    }
  if (auto current = manager.GetCurrentSession ())
    {
      return current;
    }
  return fallback;
}

static void DescribeSession (GlobalSystemMediaTransportControlsSession const &session, bool saveThumbnail, bool sendToDock, bool forceArt = false)
{
  // .get() blocks a threadpool thread; safe only because this app runs an
  // MTA apartment and the async completes on the threadpool.
  auto properties = session.TryGetMediaPropertiesAsync ().get ();
  auto playbackInfo = session.GetPlaybackInfo ();

  std::wstring app = FriendlyAppName (std::wstring{ session.SourceAppUserModelId () });
  // Web players (open.spotify.com, YouTube Music, ...) are attributed by SMTC
  // to the browser, so the dock can only show "Chromium"/"Brave". When the
  // media source is a browser, MEDIADOCK_APP_NAME lets the user label it
  // (e.g. "Spotify") without a protocol change.
  if (app == L"Chromium") // Brave (YouTube etc.) keeps its own name
    {
      wchar_t overrideName[64]{};
      const DWORD overrideLen = GetEnvironmentVariableW (L"MEDIADOCK_APP_NAME", overrideName, static_cast<DWORD> (std::size (overrideName)));
      if ((overrideLen > 0U) && (overrideLen < std::size (overrideName)))
        {
          app = overrideName;
        }
    }
  auto               title = properties.Title ();
  auto               artist = properties.Artist ();
  auto               album = properties.AlbumTitle ();
  const char        *status = PlaybackStatusName (playbackInfo.PlaybackStatus ());

  {
    std::scoped_lock const lock (g_signatureMutex);
    g_lastSignature = std::wstring (session.SourceAppUserModelId ()) + L"|" + std::wstring (title) + L"|" + std::wstring (artist)
                      + L"|" + std::wstring (album) + L"|" + std::wstring (status, status + std::char_traits<char>::length (status));
  }

  std::wcout << L"  App             : " << app << L"\n";
  std::wcout << L"  Title           : " << title << L"\n";
  std::wcout << L"  Artist          : " << artist << L"\n";
  std::wcout << L"  Album           : " << album << L"\n";
  std::wcout << L"  Playback status : " << status << L"\n";

  // Position / duration go on the wire in the metadata frame; the 1 Hz
  // ticker in main() re-anchors the dock's progress bar afterwards. Some
  // sources don't provide a timeline -> unknown sentinels (0xFFFFFFFF).
  // Position / duration go on the wire in the metadata frame; the 1 Hz
  // ticker in main() re-anchors the dock's progress bar afterwards. Some
  // sources don't provide a timeline -> unknown sentinels (0xFFFFFFFF).
  // WirePosition extrapolates the SMTC anchor while Playing; see its comment.
  const auto [positionMs, durationMs] = WirePosition (session);
  std::wcout << L"  Position        : ";
  if (positionMs == 0xFFFFFFFFU)
    {
      std::wcout << L"unknown\n";
    }
  else
    {
      std::wcout << positionMs / 1000 << L"s / " << durationMs / 1000 << L"s\n";
    }

  if (!sendToDock)
    {
      return; // listing a non-active session: print only, never touch the dock
    }

  // The wire format is UTF-8; SendToDock converts only when a port is open.
  SendToDock (app, title, artist, album, status, positionMs, durationMs);

  // The thumbnail never changes within a track, so it is only sent on the
  // paths that pass saveThumbnail=true (initial enumeration + track change).
  // SendThumbnailToDock additionally dedupes on unchanged art bytes.
  // --no-thumbnail skips the whole path - even the fetch and scale.
  if (saveThumbnail && g_sendThumbnails)
    {
      SendThumbnailIfChanged (properties.Thumbnail (), forceArt);
    }

  std::wcout.flush (); // redirected output is block-buffered otherwise
}

// Same as DescribeSession, but session APIs can throw hresult_error (e.g. the
// source app vanished mid-query); report and keep watching instead of dying.
static void DescribeSessionSafe (GlobalSystemMediaTransportControlsSession const &session, bool saveThumbnail, bool sendToDock, bool forceArt = false)
{
  try
    {
      DescribeSession (session, saveThumbnail, sendToDock, forceArt);
    }
  catch (hresult_error const &e)
    {
      std::wcout << L"  <error: " << e.message () << L">\n";
    }
  catch (...)
    {
      // A hostile session must never take the watcher down; non-WinRT
      // exceptions (e.g. bad_alloc) are outside hresult_error.
      std::wcout << L"  <error: unknown exception while describing session>\n";
    }
}

// Keeps strong references + event revokers for every live media session.
// Clearing the vector revokes all per-session event subscriptions.
using SessionEventRevoker = winrt::event_revoker<Windows::Media::Control::IGlobalSystemMediaTransportControlsSession>;
struct SessionWatcher
{
  // NB: SMTC classes are not default-constructible in modern SDKs,
  // so the member must be explicitly initialized with nullptr.
  GlobalSystemMediaTransportControlsSession session{ nullptr };
  SessionEventRevoker                       mediaRevoker;
  SessionEventRevoker                       playbackRevoker;
};
// NB: cleared/rebuilt from the SessionsChanged handler, which runs on a
// threadpool thread. SMTC coalesces manager events today, so concurrent
// mutations are not expected; add a mutex if that ever changes.
static std::vector<SessionWatcher> s_watchers;

// Re-enumerates all sessions, prints them and (re)attaches per-session events.
static void AttachSessionWatchers (GlobalSystemMediaTransportControlsSessionManager const &manager)
{
  s_watchers.clear ();

  auto sessions = manager.GetSessions ();
  std::wcout << Timestamp () << L"Media sessions: " << sessions.Size () << L"\n";

  // Both per-session events do the same thing: re-describe the session.
  // They differ only in the banner text and whether the thumbnail is saved.
  auto describeOnChange = [] (wchar_t const *banner, bool saveThumbnail)
    {
      return [banner, saveThumbnail] (GlobalSystemMediaTransportControlsSession const &sender, auto &&)
        {
          (void)sender;
          std::wcout << Timestamp () << banner << L":\n";
          // Any session event may mean "another app is now the interesting
          // one", so re-evaluate instead of trusting the event sender.
          const auto best = SelectBestSession (g_manager);
          if (best)
            {
              // Metadata events carry saveThumbnail=true: force the art out,
              // because the dock dropped its cover for the new track and the
              // image bytes may be identical to the previous track.
              DescribeSessionSafe (best, saveThumbnail, true, saveThumbnail);
            }
        };
    };

  for (auto &&session : sessions)
    {
      std::wcout << L"  ----------------------------------------\n";
      DescribeSessionSafe (session, true, false);

      SessionWatcher watcher;
      watcher.session = session;
      watcher.mediaRevoker
          = session.MediaPropertiesChanged (auto_revoke, describeOnChange (L"Now playing changed", true));
      watcher.playbackRevoker
          = session.PlaybackInfoChanged (auto_revoke, describeOnChange (L"Playback info changed", false));
      s_watchers.push_back (std::move (watcher));
    }

  // Mirror the session that is actually playing (if any) to the dock.
  if (const auto best = SelectBestSession (manager))
    {
      DescribeSessionSafe (best, true, true);
    }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main (int argc, char **argv)
{
  // Flags are order-free and combinable. Unknown flags are reported and
  // ignored rather than fatal: this is a companion app, not a CLI tool.
  bool oneShot = false;
  for (int i = 1; i < argc; ++i)
    {
      const std::string arg{ argv[i] };
      if (arg == "--oneshot")
        {
          oneShot = true;
        }
      else if (arg == "--no-thumbnail")
        {
          g_sendThumbnails = false;
        }
      else
        {
          std::wcout << L"unknown argument ignored: " << std::wstring (arg.begin (), arg.end ())
                     << L" (known: --oneshot, --no-thumbnail)\n";
        }
    }

  // UTF-8 console output so unicode titles print correctly
  // (also makes output readable when redirected to a file/pipe).
  _setmode (_fileno (stdout), _O_U8TEXT);
  SetConsoleOutputCP (CP_UTF8);

  // COM in multi-threaded mode: required for WinRT (C++/WinRT's
  // init_apartment calls RoInitialize) and for CoCreateInstance below.
  init_apartment (apartment_type::multi_threaded);

  std::wcout << L"=== MediaDock companion app ===\n\n";

  std::wcout << L"Audio devices:\n";
  ListSpeakers ();
  std::wcout << L"\n";

  try
    {
      // Auto-detect the dock's CDC port (VID_CAFE&PID_4005); COM4 remains the
      // fallback for a UART bridge setup. MEDIADOCK_COM_PORT overrides both.
      std::wstring portName = findDockPort ();
      if (portName.empty ())
        {
          portName = L"COM4";
        }
      std::wcout << L"Dock port: " << portName << L"\n";
      g_comPort = winrt::handle{ openSerialPort (portName) };
    }
  catch (std::exception const &e)
    {
      // No dock attached is a normal situation; keep running headless.
      std::wcout << L"Serial port not available (" << e.what () << L"), running without dock output\n";
    }

  // Global "now playing" hub: every app that integrates with the Windows
  // media flyout (Spotify, Edge, Chrome, Windows Media Player, ...) shows
  // up here as a session. Kept in g_manager for the position ticker below.
  g_manager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync ().get ();

  try
    {
      AttachSessionWatchers (g_manager);
    }
  catch (hresult_error const &e)
    {
      std::wcout << L"  <error: " << e.message () << L">\n";
    }

  // 1 Hz position refresh: SMTC timeline events are irregular (some sources
  // never tick at all), so the dock interpolates the progress bar between
  // these cheap position-only frames. Exceptions from dead sessions are
  // swallowed - the next tick retries.
  // One 1 Hz dock update on the main thread: SMTC calls need the apartment
  // initialized above, and a plain std::thread has none (its WinRT calls
  // threw, so the dock never got position frames or track changes).
  auto tickDock = [] ()
    {
      {
        std::scoped_lock const lock (g_serialMutex);
        EnsureDockConnectedLocked ();
      }
      try
        {
          if (auto session = SelectBestSession (g_manager))
            {
              // SMTC change events are not dependable, so compare a metadata
              // signature every second and resend (with art) when it differs.
              bool changed = false;
              try
                {
                  auto const   props = session.TryGetMediaPropertiesAsync ().get ();
                  const char  *status = PlaybackStatusName (session.GetPlaybackInfo ().PlaybackStatus ());
                  std::wstring sig = std::wstring (session.SourceAppUserModelId ()) + L"|" + std::wstring (props.Title ()) + L"|"
                                     + std::wstring (props.Artist ()) + L"|" + std::wstring (props.AlbumTitle ()) + L"|"
                                     + std::wstring (status, status + std::char_traits<char>::length (status));
                  std::scoped_lock const lock (g_signatureMutex);
                  if (sig != g_lastSignature)
                    {
                      g_lastSignature = sig;
                      changed = true;
                    }
                }
              catch (...)
                { /* properties vanished mid-query: nothing to compare */
                }
              // Retry art a few times when the new track thumbnail was not
              // ready yet; SendThumbnailToDock dedupes, so a good copy is
              // never rewritten.
              bool needArt = false;
              {
                std::scoped_lock const lock (g_signatureMutex);
                if (!changed && (g_artSignature != g_lastSignature))
                  {
                    if (g_artAttemptSignature != g_lastSignature)
                      {
                        g_artAttemptSignature = g_lastSignature;
                        g_artAttempts = 0;
                      }
                    if (g_artAttempts < 5)
                      {
                        g_artAttempts++;
                        needArt = true;
                      }
                  }
              }
              if (changed || needArt)
                {
                  std::wcout << Timestamp () << (changed ? L"Metadata changed (ticker):\n" : L"Art retry:\n");
                  // changed: the dock dropped the cover for the new track, so
                  // resend even when the pixels are identical.
                  DescribeSessionSafe (session, true, true, changed);
                }

              // Position frames only while playing; while paused the dock
              // keeps its bar frozen at the last anchored position.
              if (session.GetPlaybackInfo ().PlaybackStatus ()
                  == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing)
                {
                  const auto [posMs, endMs] = WirePosition (session);
                  SendPositionToDock (posMs, endMs);
                }
            }
        }
      catch (...)
        { /* no timeline / session vanished mid-query: retry next tick */
        }
    };

  if (!oneShot)
    {
      std::wcout << L"\nListening for changes... (press Enter to quit)\n\n";

      // New player appeared / went away -> rebuild our watcher list.
      g_manager.SessionsChanged (
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

      // Focus switched to another player (e.g. you started a video).
      g_manager.CurrentSessionChanged (
          [] (GlobalSystemMediaTransportControlsSessionManager const &sender, auto &&)
            {
              try
                {
                  const auto best = SelectBestSession (sender);
                  if (best)
                    {
                      std::wcout << Timestamp () << L"Current session changed:\n";
                      DescribeSessionSafe (best, false, true);
                    }
                }
              catch (hresult_error const &e)
                {
                  std::wcout << L"  <error: " << e.message () << L">\n";
                }
            });

      // 1 Hz loop on this (apartment-initialized) thread: keeps the dock's
      // progress bar moving and re-sends metadata when the track changes.
      // Interactive runs also quit on Enter; headless runs (stdin not a TTY)
      // keep going until the process is stopped externally.
      const bool interactive = _isatty (_fileno (stdin)) != 0;
      for (;;)
        {
          std::this_thread::sleep_for (std::chrono::seconds (1));
          tickDock ();
          if (interactive && (_kbhit () != 0))
            {
              (void)_getch ();
              break;
            }
        }
    }

  // Close under the mutex: an SMTC callback may still be inside SendToDock
  // at this point, and the handle must not be freed under it.
  std::scoped_lock const portLock (g_serialMutex);
  g_comPort.close ();
  return 0;
}
