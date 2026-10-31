#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// ---------------------------------------------------------------------------
// Media metadata link protocol (WinApp -> LCD dock over serial).
// Decoder-oriented documentation with a worked example: WinApp/info/protocol.md
//
// Wire frames (two kinds):
//
//   data frame (metadata / position):
//     [ 0x55 0xAA ][ len:3 LE ][ COBS( payload ) ][ 0x00 delimiter ]
//
//   art frame (album thumbnail):
//     [ 0x55 0xAA ][ len:3 LE ][ 0x00 0x00 ][ width:2 LE ][ height:2 LE ]
//     [ raw RGB565 pixels, width*height*2 bytes ][ 0x00 delimiter ]
//
//   The 0x00 0x00 kind marker right after the length field identifies an
//   art frame - catch it THERE, never by frame length (art size varies
//   with the aspect ratio: 160x88 art is 28 160 pixel bytes). A data
//   frame cannot contain 0x00 0x00 at that position: its COBS block
//   starts with a nonzero code byte and never contains 0x00 at all.
//
//   len: 3 bytes, little-endian. Counts the body between the marker and
//   the delimiter: the COBS block size for data frames, 4 +
//   width*height*2 (w/h header + pixels) for art frames. It excludes the
//   preamble, the length field itself, the kind marker and the delimiter.
//   It lets a memory-limited receiver size - or reject - the frame BEFORE
//   storing a single byte, so no "max protocol frame" buffer is needed.
//   The length bytes are raw and MAY contain 0x00: after the preamble,
//   framing is length-based, so they cannot be mistaken for a delimiter.
//   There is no protocol ceiling on frame size (the 3-byte field reaches
//   16 MB; data-frame TLV values still cap at 65535) - the receiver
//   applies its own sanity limit.
//
//   data payload = TLV fields (see below) followed by a 2-byte CRC-16
//   (little-endian) computed over the TLV bytes only - i.e. over
//   everything COBS encodes except the CRC itself.
//
//   art payload = RAW BYTES on purpose: no COBS, no TLV, no CRC, so the
//   MCU can shift the pixels to the panel with zero decode work. Because
//   art frames carry no CRC, the receiver sanity-checks instead: len must
//   equal 4 + width*height*2 and width/height must fit the display - that
//   rejects garbage re-locks inside pixel data (raw pixels can contain
//   any byte pattern, 0x55 0xAA included). Pixel corruption is cosmetic;
//   the art is re-sent on the next art change.
//
//   - COBS guarantees no 0x00 inside the encoded block, and the trailing
//     0x00 delimiter is a checked end marker and resync aid (the length
//     already tells the receiver where the frame ends).
//   - The 0x55 0xAA preamble is NOT needed to find frame boundaries; it
//     lets a receiver that woke up mid-frame (dock boot, USB re-enum glitch)
//     lock on immediately instead of waiting out a multi-KB art frame.
//
// Payload:  TLV fields, back to back, in tag order:
//             tag:   1 byte  (see fieldTag below)
//             len:   2 bytes, little-endian, length of <value> only
//             value: len bytes. Tags 1..5 carry UTF-8 text, NOT
//                    NUL-terminated (the length delimits it). Tags 6 and
//                    above carry opaque binary with the layouts below.
//
// Binary field layouts:
//   tag 6 (position):  fixed 8 bytes: position_ms then duration_ms, each a
//                      uint32 little-endian. 0xFFFFFFFF means unknown
//                      (e.g. the source app reports no timeline).
// Art payload (art frame body after the w/h header):
//                      Raw RGB565 (5-6-5), BIG-ENDIAN per pixel (MSB
//                      first - the order a bare SPI panel shifts out, so
//                      the MCU can DMA the buffer straight to the
//                      display), row-major, top-left first, exactly
//                      width*height*2 bytes. The sender scales art to fit
//                      within 160x160, aspect preserved (16:9 art ->
//                      160x88; 160x160 = 51 200 pixel bytes max). RGB565
//                      is the only encoding. Art never rides in data
//                      frames - encodeThumbnailFrame() builds the raw art
//                      frame, so a multi-KB art blob cannot delay text
//                      metadata on the link. (Historical: art used to be
//                      tag 7 inside the TLV; that tag value is retired
//                      and never reused.)
//
// Fields are length-delimited, so titles / artists / albums of ANY length
// travel intact: there are no fixed-size char arrays to overflow or
// truncate. The format ceiling is 65535 bytes per field (2-byte length);
// encode() clamps at that bound.
//
// CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflection, no final
// XOR. Computed over the raw TLV payload (before COBS, CRC itself excluded),
// appended low byte first. Known answer: CRC("123456789") = 0x29B1.
//
// Receiver rules (the firmware side):
//   - hunt for the 0x55 0xAA preamble (this also re-aligns after garbage)
//   - read the 3-byte length
//   - look at the next 2 bytes: 0x00 0x00 -> art frame (below); anything
//     else is already the start of the data frame's COBS block
//   - art frame: sanity-check len == 4 + width*height*2 with w/h within
//     panel bounds FIRST (art carries no CRC - this check is the garbage-
//     lock rejection), then stream exactly width*height*2 raw pixel bytes
//     to the display and verify the trailing 0x00 delimiter
//   - data frame, if len exceeds your buffer: count-and-discard that many
//     bytes and re-hunt - never store what you cannot hold
//   - data frame: collect exactly len bytes (the COBS block), then verify
//     the trailing 0x00 delimiter (sanity check; a mismatch means lost
//     sync: re-hunt)
//   - COBS-decode the collected block (bounds-check while writing), then
//     verify the CRC over all but the last 2 decoded bytes BEFORE walking
//     anything: a corrupted COBS code byte decodes to plausible-looking
//     garbage, only the CRC tells you
//   - CRC bad: drop the frame and re-hunt for the preamble
//   - CRC good: walk tag/len/value; unknown tag: skip len bytes (forward
//     compatible protocol)
//   - field longer than your buffer: skip it or drop the frame
//
// ---------------------------------------------------------------------------
enum class fieldTag : uint8_t
{
  app = 1,
  title = 2,
  artist = 3,
  album = 4,
  playbackStatus = 5,
  position = 6,
  // 7 is RETIRED (was thumbnail): art travels in its own raw art frame,
  // never as a TLV field. The value stays reserved, never reused.
};

// ---------------------------------------------------------------------------
// Builds the TLV payload from its metadata fields and COBS-encodes it into
// a framed byte buffer ready to be written to the serial port.
//
// Plain value members only (std::string / std::vector) - copies and moves
// work and there is no manual lifetime management anywhere.
//
// Not thread-safe: SMTC events arrive on threadpool threads, so serialize
// access if fields are set from more than one thread.
// ---------------------------------------------------------------------------
class serializer
{
public:
  serializer () = default;

  // Metadata setters. Stored as std::string: no fixed-size field, nothing
  // is truncated here (only the 65535-byte protocol ceiling exists, and
  // only clamped at encode time).
  void setApp (std::string_view src)
  {
    app_ = src;
  }
  void setTitle (std::string_view src)
  {
    title_ = src;
  }
  void setArtist (std::string_view src)
  {
    artist_ = src;
  }
  void setAlbum (std::string_view src)
  {
    album_ = src;
  }
  void setPlaybackStatus (std::string_view src)
  {
    playbackStatus_ = src;
  }

  // Position is part of the metadata frame (tag 6), updated every second
  // from the 1 Hz ticker in main.cpp. Unknown = 0xFFFFFFFF sentinels.
  void setPosition (uint32_t positionMs, uint32_t durationMs)
  {
    positionMs_  = positionMs;
    durationMs_ = durationMs;
  }

  // Album art pixels: raw RGB565 (5-6-5), BIG-ENDIAN per pixel, row-major.
  // Must be exactly width*height*2 bytes - encodeThumbnailFrame() refuses
  // anything else. Held until the next setThumbnail; only
  // encodeThumbnailFrame() ever puts them on the wire (as the raw art
  // frame).
  void setThumbnail (std::span<const uint8_t> rgb565, uint16_t width, uint16_t height)
  {
    thumbnail_.assign (rgb565.begin (), rgb565.end ());
    thumbWidth_  = width;
    thumbHeight_ = height;
  }

  // Assemble the TLV payload, COBS-encode it and frame it (preamble +
  // 3-byte length + COBS block + 0x00 delimiter). Clears the previous
  // buffer first (safe to call repeatedly). Returns buffer().size(), i.e.
  // the exact number of bytes to write to the port.
  [[nodiscard]] size_t encode ();

  // Raw art frame: [0x55 0xAA][len:3][0x00 0x00 kind marker][width:2 LE]
  // [height:2 LE][raw pixels][0x00 delimiter] - no COBS, no TLV, no CRC
  // (see the header comment). len:3 = 4 + width*height*2. Returns 0 and
  // leaves the buffer invalid when the pixels are empty or don't match
  // width*height*2. The header must agree with the pixels, so this
  // refuses instead of clamping: a truncated image with a full-size
  // header would misframe the dock's blit.
  [[nodiscard]] size_t encodeThumbnailFrame ();

  // Single-field frame carrying only the position (tag 6), for the 1 Hz
  // ticker: the dock interpolates between these, no text tags needed.
  [[nodiscard]] size_t encodePositionFrame ();

  // Encoded frame; only meaningful while bufferValid() is true.
  [[nodiscard]] const std::vector<uint8_t> &buffer () const
  {
    return outBuffer_;
  }
  [[nodiscard]] bool bufferValid () const
  {
    return bufferValid_;
  }

  // Protocol CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflection,
  // no final XOR) - the same CRC that rides in data frames. Exposed so
  // sender-side logs can print a checksum the dock independently
  // recomputes and compares (e.g. over the art frame body:
  // [width:2 LE][height:2 LE][raw pixels]).
  [[nodiscard]] static uint16_t crc16 (const uint8_t *data, size_t n);

private:
  static void appendField (std::vector<uint8_t> &payload, fieldTag tag, const std::string &value);
  static void appendRawField (std::vector<uint8_t> &payload, fieldTag tag, const uint8_t *value, size_t len);
  void        finishFrame (std::vector<uint8_t> &payload);
  static void     cobsAppend (const uint8_t *data, size_t n, std::vector<uint8_t> &out);

  std::string          app_;
  std::string          title_;
  std::string          artist_;
  std::string          album_;
  std::string          playbackStatus_;
  uint32_t             positionMs_  = 0xFFFFFFFF; // unknown
  uint32_t             durationMs_ = 0xFFFFFFFF; // unknown
  std::vector<uint8_t> thumbnail_;
  uint16_t             thumbWidth_  = 0;
  uint16_t             thumbHeight_ = 0;
  std::vector<uint8_t> outBuffer_;
  bool                 bufferValid_ = false;
};
