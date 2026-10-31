# Dock parser / writeback review — "it sends the same thing"

Verdict on the observed writeback stream (7 identical lines at 1 Hz):

```
03060803c97f01049d3d080324c2   (COBS block, 14 bytes)
```

**The dock's writeback is byte-faithful. This is NOT a receiver bug — WinApp
really did send the same frame every second.**

## Evidence chain

1. Standard COBS decode of the block (every code < 0xFF appends an implicit
   `0x00` while input remains) yields the 13-byte payload:
   `06 08 00 c9 7f 00 00 9d 3d 08 00 24 c2`
2. Parsed: tag=6 (position), len=8, `pos = 0x00007FC9` = **32,713 ms**,
   `dur = 0x00083D9D` = **539,037 ms (8:59)**, CRC-16/CCITT-FALSE = `0xC224`.
3. CRC over the TLV bytes recomputed = `0xC224` → **MATCH**. The frame on the
   wire is intact and well formed.
4. Seven identical blocks (same CRC too) = the position value did not advance
   for seven 1 Hz ticks while playing.

Root cause (WinApp side, fixed): `GetTimelineProperties().Position()` is an
**anchor** apps set on track change/seek, not a running clock. WinApp sampled
it raw at 1 Hz, so it kept resending 32,713 ms. Since the dock re-anchors its
progress bar on every frame, the bar froze. WinApp now extrapolates the anchor
by the wall time since `LastUpdatedTime()` while the session is Playing
(`WirePosition()` in `src/main.cpp`).

Sender-side visibility: every 1 Hz frame is now logged as
`serial: sent position <pos>/<dur> ms, cobs=<hex>` — the hex is the COBS
block exactly as the dock's writeback prints it, so the two logs can be
diffed byte for byte.

## Receiver parser bugs (cause frame LOSS, not corruption)

Found in the dock's capture task (USB CDC, TinyUSB):

1. **The trailing `0x00` frame delimiter is never consumed.** After a
   successful capture the FIFO still holds `00`; the next preamble read gets
   `00 55`, fails the `55 AA` check and takes the `tud_cdc_read_flush()`
   path — discarding the *entire* frame behind it. Roughly every other frame
   is lost this way. Fix: read and verify the delimiter byte explicitly
   (mismatch = lost sync, re-hunt), then proceed.

2. **`tud_cdc_read_flush()` on failure is destructive.** A short read or a
   preamble mismatch throws away ALL pending bytes, including complete
   frames queued behind a partial one. Fix: never flush on parse failure;
   keep the buffer and re-hunt byte-wise.

3. **The preamble hunt is 2-byte aligned** (`goto start` reads 2 bytes per
   attempt). Once the stream is off by an odd byte, `55 AA` straddles two
   reads and can never match → permanent loss of sync. Conversely `55 AA`
   can legally occur inside a COBS block (COBS forbids only `0x00`), so a
   misaligned hunt can false-match on payload data and derive a garbage
   `payloadLen`. Fix: hunt one byte at a time (shift-register style).

4. **The payload read loop spins without bound**:
   `while (read < payloadLen) read += tud_cdc_read(...)` never times out or
   yields. If the host stalls mid-frame this loop never exits. Fix: bound
   the wait (timeout) and treat expiry as lost sync.

5. **Task-notification gating can strand data.** After any `continue`, the
   task blocks in `ulTaskNotifyTakeIndexed(..., portMAX_DELAY)` even when
   bytes are already buffered; combined with (1)+(2) those bytes are later
   flushed. Fix: drain `tud_cdc_available()` completely before blocking.

The writeback printer itself (`snprintf(&line[2*i], 3, "%02x", ...)`) is
correct: each call writes 2 digits + NUL and the next call overwrites the NUL.

## Note on COBS decoding (for the firmware decoder)

Two easy mistakes, both made while triaging this:
- After a code byte `C < 0xFF`, a `0x00` is appended **whenever input
  remains**, not only for code `0x01`. Forgetting the implicit zeros after
  non-`0x01` codes drops bytes silently.
- The check is on *input remaining*, so the final code never appends a zero.

CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflect, no final XOR) is
computed over the TLV bytes only, appended low byte first. Known answer:
`crc16("123456789") = 0x29B1` — verify any decoder implementation against it
before trusting its output (a wrong bitwise implementation triaged this very
session produced a false "CRC mismatch").
