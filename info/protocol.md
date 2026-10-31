# MediaDock serial protocol (WinApp → LCD dock)

Everything the firmware needs to decode the stream from the Windows
companion app. The authoritative machine-readable spec is the header comment
in `WinApp/inc/serializer.hpp`; this document is the decoder-oriented
version, with worked examples.

Status: wire format v5 (raw art frames marked by `0x00 0x00` right after
the length; art is no longer a TLV field. No firmware exists yet, so there
is no back-compat burden).

---

## 1. Physical layer

- UART: **115200 baud, 8N1, no flow control**.
- Sender: WinApp opens `COM4`, writes whole frames; partial writes are
  possible (write timeouts) — see §9.
- Rough budget: 115200/10 ≈ **11.5 KB/s**. A metadata frame is tens of
  bytes; an art frame is 28-51 KB of raw pixels (160x88 = 28 KB, 160x160 =
  51 KB, ~2.5-4.5 s); a position frame is 20 bytes.

## 2. Frame layout

Two frame kinds. **Data frame** (metadata / position):

```
        ┌──────────┬───────────┬─────────────────────────────────┬──────┐
        │ 55 AA    │ len (3)   │  COBS( TLV payload ‖ CRC-16 )   │  00  │
        └──────────┴───────────┴─────────────────────────────────┴──────┘
         preamble   size, LE    encoded block, never contains 00   delimiter
```

**Art frame** (album thumbnail):

```
        ┌──────────┬───────────┬────────┬──────────┬──────────┬─────────────┬──────┐
        │ 55 AA    │ len (3)   │ 00 00  │ w (2 LE) │ h (2 LE) │ raw RGB565  │  00  │
        └──────────┴───────────┴────────┴──────────┴──────────┴─────────────┴──────┘
         preamble   size, LE    marker   blit size  w*h*2 pixel bytes     delimiter
```

| Piece | Size | Meaning |
|---|---|---|
| Preamble | 2 | Fixed `0x55 0xAA`. |
| Length | 3 | Little-endian count of the **body bytes that follow the marker** (excludes preamble, length field, marker, delimiter). Data frames: the COBS block size. Art frames: `4 + w*h*2` (w/h header + pixels). Raw bytes — **may contain `0x00`**, which is fine: after the preamble, framing is length-based. No protocol ceiling (16 MB field); the receiver applies its own sanity limit. |
| Kind marker | 0 or 2 | `0x00 0x00` **identifies an art frame — catch it here, never by frame length** (art size varies with the aspect ratio). A data frame cannot have `0x00 0x00` at this position: its COBS block starts with a nonzero code byte and never contains `0x00` at all. |
| Body | variable | Data frame: COBS-encoded `payload`, see §4. Art frame: **raw bytes** — `w:2 LE`, `h:2 LE`, then `w*h*2` raw RGB565 pixels (no COBS, no TLV, no CRC). |
| Delimiter | 1 | Fixed `0x00`. Checked end marker. |

The data-frame payload is:

```
payload = TLV-fields ‖ CRC16-LE
```

i.e. the CRC is **inside** the COBS block, appended after the TLV fields,
low byte first, computed over the TLV bytes only (not over the CRC bytes
themselves, not over COBS/sync/delimiter).

Art frames deliberately skip COBS/TLV/CRC so the MCU can shift the pixels
to the panel with zero decode work. They therefore carry no CRC —
sanity-check instead: `len == 4 + w*h*2` and `w`/`h` within panel bounds.
That check *is* the integrity check; it rejects garbage re-locks inside
pixel data (raw pixels can contain any byte pattern, `55 AA` included).
Pixel corruption itself is cosmetic — the next track re-sends the art.

### Why a preamble, a length AND a delimiter?

- The length is the workhorse for memory-limited receivers: you know the
  frame size **before storing a single byte**, so you can size your buffer
  for what you actually accept and count-and-discard bigger frames — no
  "reserve for the largest frame in the protocol" buffer.
- The `0x00` delimiter is a cheap *checked end marker*: the byte after the
  declared length must be `0x00`, and a mismatch means lost sync — re-hunt.
  It also keeps a `0x00`-based resync path for host-side debug tools.
- The preamble is for *fast startup lock*: a receiver that wakes up mid-frame
  (dock boot, USB re-enumeration glitch) would otherwise have to wait out a
  whole multi-KB thumbnail frame before seeing a boundary. Hunting for
  `0x55 0xAA` re-locks immediately and doubles as a byte-alignment sanity
  check.
- Do **not** scan for `0x00` to find frame ends: the length bytes are raw
  and may contain zeros. After the preamble, trust the length.

## 3. CRC-16

- **CRC-16/CCITT-FALSE**: poly `0x1021`, init `0xFFFF`, no reflection, no
  final XOR-out. Width 16.
- Computed over the **raw TLV payload** (before COBS, CRC bytes excluded).
- Transmitted **little-endian** (low byte first) at the end of the payload.
- Known answer: `CRC16("123456789") = 0x29B1` — use this to test your
  implementation first.

Reference C (works for both sides; the MCU only ever verifies):

```c
uint16_t crc16_ccitt (const uint8_t *data, size_t n)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < n; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021)
                                 : (uint16_t)(crc << 1);
    }
    return crc;
}
```

Bitwise, no table: worst case ≈ 520 K inner steps for a max-size data
frame — negligible even at 100 MHz, zero table RAM. (Art frames skip the
CRC entirely — see §2.)

## 4. COBS in 60 seconds

COBS (Consistent Overhead Byte Stuffing) removes `0x00` bytes from the
payload so `0x00` can be the frame delimiter. The encoder splits the payload
into blocks that end just before a `0x00` (or after 254 bytes, whichever
first) and prefixes each block with a *code byte* = (bytes in block) + 1.
The `0x00` bytes themselves are not transmitted — they are **implicit** at
each block end.

Decoding: read code `c`, copy `c-1` bytes, then emit one `0x00` if `c < 0xFF`
and more input follows; repeat. Example — payload `01 07 00 41` encodes to
`03 01 07 02 41`:

```
03          -> block of 2: 01 07, then implicit 00
02 41       -> block of 1: 41, then implicit 00 (last block: no trailing 00)
```

Pitfalls:

- The transmitted block **starts with the first code byte**; when parsing,
  remember the 5 framing bytes (2 preamble + 3 length) come before it.
- A corrupted code byte can make the decoder emit a different number of
  bytes than the sender encoded. **Always bounds-check the output buffer**
  (size it for input length + input/254 + 2) and **verify the CRC before
  trusting anything you decoded**.

## 5. TLV payload

After CRC verification, walk the payload as back-to-back fields:

```
[tag: 1 byte][len: 2 bytes, little-endian][value: len bytes]
```

- `len` counts **value bytes only** (not the 3-byte header).
- Values are NOT NUL-terminated; the length delimits them.
- Ceiling: 65535 bytes per field (the 2-byte length). The sender clamps
  text fields, never wraps. (Art is not a TLV field and does not pay this
  ceiling — see §2.)

| Tag | Field | Value layout |
|---|---|---|
| 1 | app | UTF-8 source app name |
| 2 | title | UTF-8 track title |
| 3 | artist | UTF-8 artist |
| 4 | album | UTF-8 album |
| 5 | playbackStatus | UTF-8: `Closed` `Opened` `Changing` `Stopped` `Playing` `Paused` |
| 6 | position | **8 bytes**: `position_ms` (uint32 LE) then `duration_ms` (uint32 LE). `0xFFFFFFFF` = unknown |
| 7 | *(retired)* | Was thumbnail. Art now travels in its own raw art frame (§2); the tag value stays reserved, never reused |

Rules:

- Text fields may be empty (len 0) — e.g. Brave reports no album.
- **Unknown tag: skip `len` bytes** and continue. This is the forward
  compatibility mechanism; new WinApp versions may add tags.
- A frame does not have to contain every tag: the 1 Hz ticker sends
  position-only frames (§7). Treat every tag as optional.
- Tag order is ascending when the sender is this WinApp, but do not rely on
  it — walk by tag/len, not by position.

## 6. Worked examples (real frames from the WinApp)

### Metadata frame (109 bytes)

Session: Spotify, "Never Gonna Give You Up" / "Rick Astley", Playing,
83.25 s / 213 s.

```
55 AA                                   preamble
67 00 00                                length = 103 COBS bytes follow
03 01 07                                blk: 01 07 + imp 00  -> app header   [01 07 00]
0A 53 70 6F 74 69 66 79 02 17           blk: "Spotify"+02 17 + imp 00 -> title header
1A 4E 65 .. 20 55 70                    blk: title text(23) + 03 0B + imp 00
0E 52 69 .. 41 04 1A                    blk: "Rick Astley"(11) + 04 1A + imp 00
1D 57 .. 6F 64 79 05 07                 blk: album text(26) + 05 07 + imp 00
0A 50 6C .. 67 06 08                    blk: "Playing"(7) + 06 08 + imp 00
04 32 45 01                             blk: 32 45 01 + imp 00   \ position value:
04 08 40 03                             blk: 08 40 03 + imp 00   / 0x014532=83250 ms, 0x034008=213000 ms
03 24 F5                                blk: CRC-16 LE = 0xF524
00                                      delimiter
```

Decoded payload (with the implicit zeros filled in):

```
01 07 00 "Spotify"
02 17 00 "Never Gonna Give You Up"
03 0B 00 "Rick Astley"
04 1A 00 "Whenever You Need Somebody"
05 07 00 "Playing"
06 08 00 32 45 01 00 08 40 03 00        position=83250 ms, duration=213000 ms
24 F5                                   CRC-16 over everything above
```

### Position-only frame (20 bytes) — the 1 Hz ticker

```
55 AA 0E 00 00 03 06 08 04 90 73 01 04 08 40 03 03 FB 7B 00
```

(`0E 00 00` = length 14.)

Decodes to a single field: `06 08 00 90 73 01 00 08 40 03 00` + CRC
`0x7BFB` → position 95120 ms / duration 213000 ms.

### Art frame (structure) — album art

Too big to hex-dump (28-51 KB of pixels), but the shape is always:

```
55 AA                       preamble
04 C8 00                    length:3 LE = 4 + w*h*2 (51204 for 160x160)
00 00                       kind marker: 0x00 0x00 = art frame
A0 00                       width  = 160 (LE)
A0 00                       height = 160 (LE)
F8 00 ...                   51200 raw pixel bytes: RGB565 BE, row-major
00                          delimiter
```

16:9 art is the same shape at `A0 00 58 00` (160x88): length `04 6E 00`
(28164), total frame 28 172 bytes. No COBS, no CRC — the pixels are raw.

## 7. What to expect on the wire (sender behavior)

| Event | Frame sent |
|---|---|
| WinApp start / new session / track change (`MediaPropertiesChanged`) | Metadata frame (tags 1–6) **and**, 50 ms later, an art frame if the art pixels changed |
| Play / pause / seek (`PlaybackInfoChanged`) | Metadata frame (tags 1–6) — re-anchors position |
| Every 1 s while `Playing` | Position-only frame (tag 6). **Nothing while paused** — freeze the bar at the last anchor |
| Art | Never inside a data frame; its own raw art frame, 50 ms after the data frame, only when pixels change |

Suggested dock behavior: a metadata frame updates the text display and the
progress bar; between updates, advance position_ms locally by 1 ms per ms
while the state says Playing. A `duration_ms` of `0xFFFFFFFF` means "no
timeline" — hide the bar.

## 8. Receiver state machine (C skeleton)

```c
/* Size this for the largest DATA frame you accept - the length field lets
   you reject bigger frames without a max-protocol-size buffer. Art frames
   stream past this buffer (or get counted off), never stored in it. */
#define MAX_COBS 2048
#define MAX_ART 51200                   /* bytes: 160x160 RGB565 */

typedef enum { HUNT, LENGTH, KIND, COLLECT, DISCARD,
               TRAILER, ART_HEAD, ART_PIXELS, ART_TRAILER } State;

void feed (uint8_t byte)
{
    static State    state = HUNT;
    static uint8_t  prev  = 0;
    static uint8_t  lbuf[3], kbuf[2], hbuf[4];
    static uint8_t  buf[MAX_COBS];
    static uint32_t len = 0, got = 0, need = 0;

    switch (state) {
    case HUNT:                          /* scan for the 55 AA preamble */
        if (prev == 0x55 && byte == 0xAA) { state = LENGTH; got = 0; }
        prev = byte;
        break;

    case LENGTH:                        /* 3 bytes LE: body size */
        lbuf[got++] = byte;
        if (got == 3) {
            len = (uint32_t)lbuf[0] | ((uint32_t)lbuf[1] << 8)
                                    | ((uint32_t)lbuf[2] << 16);
            got = 0;
            state = KIND;
        }
        break;

    case KIND:                  /* body starts with 00 00? -> art frame */
        kbuf[got++] = byte;
        if (got < 2) break;
        got = 0;
        if (kbuf[0] == 0x00 && kbuf[1] == 0x00) {          /* art frame */
            state = (len >= 4 && len <= 4 + MAX_ART) ? ART_HEAD : DISCARD;
        } else if (len >= 3 && len <= MAX_COBS) {          /* data frame */
            /* those 2 bytes were already its first COBS bytes */
            buf[0] = kbuf[0]; buf[1] = kbuf[1]; got = 2;
            state = COLLECT;
        } else {                               /* garbage length: drop it */
            got = 2;                    /* body bytes already taken so far */
            state = DISCARD;
        }
        break;

    case COLLECT:                       /* exactly len bytes: no guessing */
        buf[got++] = byte;
        if (got == len) state = TRAILER;
        break;

    case DISCARD:                  /* too big / garbage: count off, drop */
        if (++got >= len) state = HUNT; /* stray 0x00 trailer is ignored */
        break;

    case TRAILER:                       /* expect the 0x00 delimiter */
        if (byte == 0x00) handle_frame (buf, len);
        state = HUNT;                   /* mismatch: lost sync, re-hunt */
        break;

    case ART_HEAD:                   /* width:2 LE, then height:2 LE */
        hbuf[got++] = byte;
        if (got < 4) break;
        got = 0;
        need = (uint32_t)(hbuf[0] | (hbuf[1] << 8)) *
               (uint32_t)(hbuf[2] | (hbuf[3] << 8)) * 2u;
        /* len == 4 + w*h*2 is THE integrity check for art frames (they
           carry no CRC): it rejects garbage re-locks inside pixel data */
        if (need == 0 || need > MAX_ART || len != 4 + need) {
            got = 4;                    /* body bytes already taken so far */
            state = (len > 4) ? DISCARD : HUNT;
            break;
        }
        art_begin (hbuf[0] | (hbuf[1] << 8), hbuf[2] | (hbuf[3] << 8));
        state = ART_PIXELS;
        break;

    case ART_PIXELS:                  /* raw RGB565 -> straight to GRAM */
        art_push (byte);                /* or feed a DMA / UART ring */
        if (++got == need) state = ART_TRAILER;
        break;

    case ART_TRAILER:                   /* expect the 0x00 delimiter */
        if (byte == 0x00) art_end ();
        state = HUNT;                   /* mismatch: lost sync, re-hunt */
        break;
    }
}

void handle_frame (const uint8_t *cobs, size_t n)
{
    static uint8_t payload[MAX_COBS];   /* COBS decode never grows: fits */
    size_t         plen = cobs_decode (cobs, n, payload, sizeof payload);
    if (plen < 2) return;                                   /* too short */

    uint16_t stored = (uint16_t)(payload[plen-2] | (payload[plen-1] << 8));
    if (crc16_ccitt (payload, plen - 2) != stored) return;  /* bad CRC: drop */

    /* walk TLV: for (size_t i = 0; i < plen - 2;) {
         tag = payload[i]; len = payload[i+1] | (payload[i+2] << 8); i += 3;
         switch (tag) { ... default: break; }   // unknown tag: just skip
         i += len;  (bounds-check first!)
       } */
}
```

Notes:

- The length check is the memory win: you never store more than `MAX_COBS`,
  and frames you can't (or don't want to) hold are counted off and dropped
  before a byte hits RAM. Art frames are the extreme case: if the dock
  displays no thumbnails, the `00 00` marker (or `len > 4096`) means
  "count `len` bytes off and re-hunt".
- Any failure (bad CRC, bad trailer, failed art sanity check) leaves the
  state machine in `HUNT`, so error recovery is automatic.
- Art frames are raw RGB565 pixels ready for the panel: if your display
  takes RGB565 over SPI (ST7789/ILI9341-style), push the pixel bytes
  straight to the GRAM as they arrive - no COBS, no CRC, no image decoder,
  no 51 KB RAM buffer. Art carries no CRC (the price of raw pixels);
  `len == 4 + w*h*2` plus sane w/h is the garbage-lock rejection, and a
  corrupted image is cosmetic (the next track re-sends the art).
- The `[width:2 LE][height:2 LE]` header sizes the blit window. The sender
  scales art to fit within 160x160, aspect preserved (16:9 art arrives as
  160x88) - center non-square results in your art box.

## 9. Error handling cheat sheet

| Symptom | Cause | Response |
|---|---|---|
| CRC mismatch | corrupted byte(s), or you lost sync inside what you thought was one frame | Drop the frame; the `HUNT` state re-locks on the next `55 AA` |
| Declared length > your buffer | thumbnail you can't hold, or garbage length after a lost sync | `DISCARD` that many bytes, then re-hunt (see 8) |
| Byte after the COBS block isn't `0x00` | lost sync inside what you thought was a frame | Drop and re-hunt on `55 AA` |
| TLV walk runs off the end | corrupted (but CRC-passing? then it's a decoder bug) or you forgot the 2-byte CRC tail | Bounds-check every read |
| Garbled image | bit errors inside an art frame (art carries no CRC) | Cosmetic; next track re-sends. A frame failing the `len == 4 + w*h*2` check is a garbage lock - drop and re-hunt |
| Text garbled only after ~N bytes | partial UART write on the PC side or baud mismatch | WinApp logs partial writes; check baud = 115200 |

## 10. Known limitations

- CRC covers the payload, not the framing bytes; a preamble byte flip just
  costs one frame (re-hunt finds the next one) and a corrupted length field
  is caught by the delimiter check or the CRC.
- Data frames have no type field — the tag set *is* the type. Art frames
  are marked by `0x00 0x00` right after the length (catch art by the
  marker, never by frame length).
- No ACK/retry: it's a one-way status link; every event re-sends current
  state, so a lost frame self-heals on the next event (worst case: thumbnail
  waits for the next track change — acceptable for a media dock).
