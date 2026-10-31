# "serial: write failed, err=121" — attribution: dock firmware, not WinApp

Scope: why `WriteFile` failing with Win32 error **121 (`ERROR_SEM_TIMEOUT`)** on
this link is a media-dock (far-end) software problem, what WinApp-side
alternative causes existed and how they are now ruled out, and how to read the
failure logs produced since this change.

## What err=121 actually is

`ERROR_SEM_TIMEOUT` on a serial `WriteFile` is raised by the **serial/USB
driver**, not by application code. It means: the write deadline
(`WriteTotalTimeoutConstant + WriteTotalTimeoutMultiplier × bytes`) expired
before the driver could hand the bytes to the device. No serializer, TLV,
frame-building or threading bug in WinApp can synthesize this error — the
WinApp payload is just bytes to the driver.

Timing on this link (115200 8N1):

| frame        | size (approx) | write deadline (100 ms + 10 ms/B) | wire time (~87 µs/B) |
|--------------|---------------|-----------------------------------|----------------------|
| position (tag 6 only) | ~13 B  | ~230 ms  | ~1.2 ms  |
| metadata (tags 1–6)   | ~100–300 B | ~1–4 s | ~10–30 ms |
| thumbnail (tag 7)     | up to ~51 KB | minutes | ~4.5 s |

A 121 on a **position** frame therefore means the device accepted (nearly) zero
bytes for a quarter second. The deadline is >100× the wire time, so ordinary
congestion cannot cause it — the dock firmware stopped servicing its RX
(UART IRQ not running, blocked LCD draw with interrupts off, USB CDC OUT
endpoint not polled, …).

## Why it shows up "on consecutive calls, especially position"

This is a *consequence* of one dock stall, not a second bug:

1. The dock stops draining RX for >230 ms. The current `WriteFile` expires,
   and the bytes the driver had queued **stay in the host output queue**.
2. Every later `WriteFile` appends behind that backlog and expires too while
   the stall lasts → consecutive `err=121` lines.
3. Position frames are the only constant (1 Hz) traffic, so they land in every
   stall window and dominate the failures. Metadata/thumbnail frames are
   event-driven and much rarer.
4. When the firmware resumes servicing RX, the queue drains and writes succeed
   again with no host-side intervention.

The new logs make this exact sequence visible (see below).

## WinApp-side alternative causes — ruled out

For the attribution to hold, every host-side cause of a write stall had to be
excluded. `src/COMPort.cpp` now pins all of them explicitly instead of
inheriting whatever `GetCommState` returns from the driver or a previous opener:

- `fOutxCtsFlow = FALSE`, `fOutxDsrFlow = FALSE`, `fDsrSensitivity = FALSE` —
  hardware handshake. The dock is a 3-wire link and never drives CTS/DSR; if
  these flags were left enabled, writes would stall host-side and fail with
  **the same err=121**. This was the only plausible "faulty WinApp" cause and
  is now impossible.
- `fOutX = FALSE`, `fInX = FALSE` — no XON/XOFF injection.
- `fAbortOnError = FALSE` (pre-existing, AGENTS.md rule).
- `fDtrControl`/`fRtsControl` = ENABLE — lines driven, never gated.

Also verified in `src/main.cpp`:

- Exactly one `WriteFile` site, always under the single `g_serialMutex`, so
  frames cannot interleave or race on the handle (no overlapping writes).
- `GetLastError()` is captured into a local **before** any stream output runs,
  so the logged code cannot be clobbered by CRT/stream calls.
- Partial writes are already detected (`written != frameSize`) and logged;
  they do not masquerade as successes.
- Serializer/TLV content is irrelevant to 121: the driver accepts the buffer
  opaquely, and a malformed frame would fail CRC *on the dock*, not here.

## Reading the new failure logs

`WriteFrameLocked` now logs frame kind, size, and `COMSTAT.cbOutQue` (bytes
still queued host-side) before and after each failed attempt:

```
serial: write failed, err=121 (ERROR_SEM_TIMEOUT: write deadline expired,
device stopped accepting bytes (dock-side RX stall)), frame=position, 13
bytes, host tx queue 1175 -> 1175 bytes
serial: write failed, err=121 (...), frame=position, 13 bytes, host tx
queue 1175 -> 1175 bytes
serial: write failed, err=121 (...), frame=metadata, 128 bytes, host tx
queue 1201 -> 1201 bytes
serial: sent frame, 24 bytes
```

How to interpret:

- **`host tx queue` high and unchanged across consecutive failures** → the
  driver is ready and full, the device drains nothing. Far-end stall. (The
  example above: the queue only grows with each appended frame.)
- **queue shrinking between failures** → the dock is draining slowly (still
  far-end, just intermittent — e.g. long LCD updates), writes resume once it
  catches up.
- **queue near 0 and still err=121** → bytes are leaving the driver but the
  line is not moving; check physical link / USB stack. Not seen with a
  healthy dock.
- `frame=position` predominance is expected (1 Hz traffic) and is not
  evidence of a position-frame bug.

## Follow-up (optional, host-side mitigation, not a fix)

If the log noise or the cascading failures become a problem, `PURGE_TXABORT |
PURGE_TXCLEAR` after a timeout would drop the stale backlog so the next frame
starts clean. That masks the stall signature above — keep the purge **out** of
the firmware debugging phase so the dock-side evidence stays visible.

Root-cause work belongs in the firmware: service the UART/USB RX path
continuously (IRQ-driven ring buffer) and never run LCD/draw code with RX
interrupts masked for more than a few milliseconds.
