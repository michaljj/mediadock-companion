// Unit tests for the serializer module (TLV/COBS data frames + raw art
// frames).
//
// Build/run: cmake --build build --config Debug --target serializer_test
//            ctest -C Debug (or run the exe directly)
// Exits non-zero on any failure.

#include "serializer.hpp"

// The firmware's own decoder, compiled as C on the host, so frames produced by
// this serializer are checked against the real receiver code, not only against
// the reference decoders in this file.
extern "C"
{
#include "protocol.h"
}

#include <crtdbg.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

static int failures = 0;

#define CHECK(cond)                                                                                                    \
  do                                                                                                                   \
    {                                                                                                                  \
      if (!(cond))                                                                                                     \
        {                                                                                                              \
          printf ("  FAIL: %s (line %d)\n", #cond, __LINE__);                                                          \
          ++failures;                                                                                                  \
        }                                                                                                              \
    }                                                                                                                  \
  while (0)

// ---------------------------------------------------------------------------
// Independent reference implementations (receiver side of the protocol).
// Deliberately NOT the module's own code: the round-trip tests below pin the
// module against these.
// ---------------------------------------------------------------------------

// Reference COBS decoder.
static std::vector<uint8_t> cobsDecode (const std::vector<uint8_t> &frame)
{
  std::vector<uint8_t> out;
  size_t               i = 0;
  while (i < frame.size ())
    {
      const uint8_t code = frame[i++];
      CHECK (code != 0); // zero inside payload would break framing
      if (code == 0)
        return out;
      for (uint8_t j = 1; j < code && i < frame.size (); ++j)
        out.push_back (frame[i++]);
      if (code < 0xff && i < frame.size ())
        out.push_back (0); // implicit zero between blocks
    }
  return out;
}

// Canonical COBS encoder (Wikipedia reference, raw preallocated buffer).
// Used only for test 6 so the unknown-tag frame is built by an independent
// implementation. (Known wart: returns one byte short if a 0xFF block
// closes exactly on the last input byte - test 6's short payload cannot
// hit that edge.)
static size_t cobsRefEncode (const uint8_t *data, size_t length, uint8_t *buffer)
{
  uint8_t *encode = buffer;
  uint8_t *codep = encode++;
  uint8_t  code = 1;
  for (const uint8_t *byte = data; length--; ++byte)
    {
      if (*byte)
        {
          *encode++ = *byte;
          ++code;
        }
      if (!*byte || code == 0xff)
        {
          *codep = code;
          code = 1;
          codep = encode;
          if (!*byte || length)
            ++encode;
        }
    }
  *codep = code;
  return static_cast<size_t> (encode - buffer);
}

// Reference CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflection,
// no final XOR. Deliberately independent of the module's implementation.
static uint16_t crc16Ref (const std::vector<uint8_t> &data)
{
  uint16_t crc = 0xFFFF;
  for (uint8_t byte : data)
    {
      crc ^= static_cast<uint16_t> (byte) << 8;
      for (int bit = 0; bit < 8; ++bit)
        {
          crc = (crc & 0x8000) ? static_cast<uint16_t> ((crc << 1) ^ 0x1021) : static_cast<uint16_t> (crc << 1);
        }
    }
  return crc;
}

// Full receiver side for data frames: check the 0x55 0xAA preamble, read
// the 3-byte length and verify it matches the COBS block that arrived,
// check the 0x00 delimiter, COBS-decode, verify the CRC over all but the
// last 2 decoded bytes, strip the CRC and return the raw TLV payload.
static std::vector<uint8_t> decodeFrame (const std::vector<uint8_t> &frame)
{
  CHECK (frame.size () >= 2 + 3 + 1 + 2 + 1);   // preamble + len:3 + COBS code byte + CRC + delimiter
  CHECK (frame[0] == 0x55 && frame[1] == 0xAA); // preamble present
  CHECK (frame.back () == 0x00);                // delimiter present

  // len:3 LE counts the COBS block bytes (excludes preamble, length field
  // and delimiter). This is the field a memory-limited receiver uses to
  // size or reject a frame before storing it - so its correctness is
  // pinned here on every round trip.
  const size_t declared
      = static_cast<size_t> (frame[2]) | (static_cast<size_t> (frame[3]) << 8) | (static_cast<size_t> (frame[4]) << 16);
  CHECK (frame.size () == 2 + 3 + declared + 1);
  if (frame.size () != 2 + 3 + declared + 1)
    return {};

  std::vector<uint8_t> payload = cobsDecode ({ frame.begin () + 5, frame.end () - 1 });
  CHECK (payload.size () >= 2); // room for the CRC
  const uint16_t stored = static_cast<uint16_t> (payload[payload.size () - 2] | (payload[payload.size () - 1] << 8));
  payload.resize (payload.size () - 2);
  CHECK (crc16Ref (payload) == stored); // CRC verifies BEFORE any TLV walk
  return payload;
}

// Full receiver side for RAW art frames: preamble, len:3, the 0x00 0x00
// kind marker, w:2 LE, h:2 LE, raw pixels, 0x00 delimiter. There is no
// COBS/TLV/CRC to decode - the pixels are taken byte-for-byte (that is
// the point of this frame). The len == 4 + w*h*2 check IS the integrity
// check on the wire: it must hold on every art frame.
struct ArtFrame
{
  uint16_t             width = 0;
  uint16_t             height = 0;
  std::vector<uint8_t> pixels;
};

static ArtFrame decodeArtFrame (const std::vector<uint8_t> &frame)
{
  ArtFrame out;
  CHECK (frame.size () >= 2 + 3 + 2 + 4 + 1);   // preamble + len:3 + marker + w/h + delimiter
  CHECK (frame[0] == 0x55 && frame[1] == 0xAA); // preamble present
  CHECK (frame.back () == 0x00);                // delimiter present
  CHECK (frame[5] == 0x00 && frame[6] == 0x00); // kind marker identifies art frames
  if (frame.size () < 2 + 3 + 2 + 4 + 1)
    return out;

  // len:3 LE counts the w/h header + pixels (excludes preamble, length
  // field, kind marker and delimiter).
  const size_t declared
      = static_cast<size_t> (frame[2]) | (static_cast<size_t> (frame[3]) << 8) | (static_cast<size_t> (frame[4]) << 16);
  CHECK (frame.size () == 2 + 3 + 2 + declared + 1);
  if (frame.size () != 2 + 3 + 2 + declared + 1)
    return out;

  out.width = static_cast<uint16_t> (frame[7] | (frame[8] << 8));
  out.height = static_cast<uint16_t> (frame[9] | (frame[10] << 8));
  CHECK (declared == 4 + static_cast<size_t> (out.width) * out.height * 2); // the art-frame integrity check
  out.pixels.assign (frame.begin () + 11, frame.end () - 1);
  CHECK (out.pixels.size () == static_cast<size_t> (out.width) * out.height * 2);
  return out;
}

// Reference TLV parser. Walks [tag:1][len:2 LE][value] fields. Unknown tags
// are skipped via their length (forward compatibility rule) and counted.
struct ParsedTlv
{
  std::map<fieldTag, std::string>         fields;
  std::map<fieldTag, std::vector<uint8_t>> binaryFields; // tag 6 (position)
  size_t                                   skippedUnknownBytes = 0;
};

static ParsedTlv parseTlv (const std::vector<uint8_t> &payload)
{
  ParsedTlv out;
  size_t    pos = 0;
  while (pos < payload.size ())
    {
      CHECK (pos + 3 <= payload.size ()); // tag + length must fit
      if (pos + 3 > payload.size ())
        break;
      const auto     tag = static_cast<fieldTag> (payload[pos]);
      const uint16_t len = static_cast<uint16_t> (payload[pos + 1] | (payload[pos + 2] << 8));
      pos += 3;
      CHECK (pos + len <= payload.size ());
      if (pos + len > payload.size ())
        break;
      if (tag == fieldTag::app || tag == fieldTag::title || tag == fieldTag::artist || tag == fieldTag::album
          || tag == fieldTag::playbackStatus)
        {
          out.fields[tag] = std::string (payload.begin () + static_cast<long> (pos),
                                         payload.begin () + static_cast<long> (pos + len));
        }
      else if (tag == fieldTag::position)
        {
          out.binaryFields[tag] = std::vector<uint8_t> (payload.begin () + static_cast<long> (pos),
                                                        payload.begin () + static_cast<long> (pos + len));
        }
      else
        {
          out.skippedUnknownBytes += 3 + len; // header + skipped value
        }
      pos += len;
    }
  return out;
}

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------

// Full round-trip of an encoded serializer: frame -> COBS decode -> TLV
// parse -> compare against the expected fields. Also asserts framing rules
// and idempotence (a second encode reproduces the identical frame).
static void roundTrip (const char *name, serializer &s, const std::map<fieldTag, std::string> &expected)
{
  const size_t n = s.encode ();

  CHECK (s.bufferValid ());
  CHECK (n == s.buffer ().size ());
  const auto &frame = s.buffer ();
  CHECK (!frame.empty () && frame.back () == 0x00); // delimiter present

  // The COBS block sits between the 5 framing bytes (2 preamble + 3
  // length) and the delimiter and must never contain 0x00. (The length
  // bytes MAY - they are raw - so they are excluded from this check.)
  const std::vector<uint8_t> payload (frame.begin () + 5, frame.end () - 1);
  for (uint8_t b : payload)
    if (b == 0x00)
      {
        CHECK (false && "zero byte inside COBS payload");
        break;
      }

  const ParsedTlv parsed = parseTlv (decodeFrame (frame));
  CHECK (parsed.fields == expected);
  CHECK (parsed.skippedUnknownBytes == 0);

  const auto   first = s.buffer ();
  const size_t n2 = s.encode ();
  CHECK (n2 == first.size ());
  CHECK (memcmp (first.data (), s.buffer ().data (), n2) == 0);

  printf ("[%s] frame=%zu %s\n", name, n, "ok");
}

static std::map<fieldTag, std::string> expectedOf (std::string app, std::string title, std::string artist,
                                                   std::string album, std::string status)
{
  return { { fieldTag::app, std::move (app) },
           { fieldTag::title, std::move (title) },
           { fieldTag::artist, std::move (artist) },
           { fieldTag::album, std::move (album) },
           { fieldTag::playbackStatus, std::move (status) } };
}


// ---------------------------------------------------------------------------
// Firmware-decoder cross-checks (Soft/user_app/src/protocol.c).
// ---------------------------------------------------------------------------

// Strip the wire envelope and hand the COBS block to the firmware decoder.
static protocolFields_t decodeWithFirmwareDecoder (const std::vector<uint8_t> &frame)
{
  protocolFields_t fields{};
  CHECK (frame.size () > 6);
  CHECK (frame[0] == 0x55 && frame[1] == 0xAA && frame.back () == 0x00);
  const size_t cobsLen
      = static_cast<size_t> (frame[2]) | (static_cast<size_t> (frame[3]) << 8) | (static_cast<size_t> (frame[4]) << 16);
  CHECK (cobsLen + 6u == frame.size ());
  CHECK (protocolDecodeDataFrame (frame.data () + 5, static_cast<uint32_t> (cobsLen), &fields) == PR_OK_u8);
  return fields;
}

// Every metadata case must survive the firmware decoder with identical values.
static void firmwareRoundTrip (const char *name, serializer s, const char *app, const char *title, const char *artist,
                               const char *album, const char *status, uint32_t positionMs, uint32_t durationMs)
{
  s.setPosition (positionMs, durationMs);
  CHECK (s.encode () > 0);
  const protocolFields_t fields = decodeWithFirmwareDecoder (s.buffer ());
  CHECK (strcmp (fields.app, app) == 0);
  CHECK (strcmp (fields.title, title) == 0);
  CHECK (strcmp (fields.artist, artist) == 0);
  CHECK (strcmp (fields.album, album) == 0);
  CHECK (strcmp (fields.playbackStatus, status) == 0);
  CHECK (fields.positionMs == positionMs);
  CHECK (fields.durationMs == durationMs);
  printf ("[firmware %s] frame=%zu ok\n", name, s.buffer ().size ());
}

int main ()
{
  // Route debug asserts to stderr instead of a modal dialog so automated
  // runs fail loudly instead of blocking on "Retry to debug".
  _CrtSetReportMode (_CRT_ASSERT, _CRTDBG_MODE_FILE);
  _CrtSetReportFile (_CRT_ASSERT, _CRTDBG_FILE_STDERR);

  try
    {

  // 1. Realistic short strings.
  {
    serializer s;
    s.setApp ("Spotify");
    s.setTitle ("Never Gonna Give You Up");
    s.setArtist ("Rick Astley");
    s.setAlbum ("Whenever You Need Somebody");
    s.setPlaybackStatus ("Playing");
    roundTrip (
        "short", s,
        expectedOf ("Spotify", "Never Gonna Give You Up", "Rick Astley", "Whenever You Need Somebody", "Playing"));
  }

  // 2. Long strings - far beyond the old 128-byte char arrays and long
  //    enough to force multiple COBS blocks incl. the 0xFF wrap path.
  {
    const std::string longTitle (400, 'a');
    const std::string longAlbum (300, 'b');
    serializer        s;
    s.setTitle (longTitle);
    s.setAlbum (longAlbum);
    roundTrip ("long", s, expectedOf ("", longTitle, "", longAlbum, ""));
  }

  // 3. Multibyte UTF-8 (length counts BYTES, not characters).
  {
    const std::string artist = "Bj\xc3\xb6rk Gu\xc3\xb0mundsd\xc3\xb3ttir \xF0\x9F\x8E\xB5";
    serializer        s;
    s.setArtist (artist);
    roundTrip ("utf8", s, expectedOf ("", "", artist, "", ""));
  }

  // 4. Everything empty: five empty TLV fields still form a valid frame.
  {
    serializer s;
    roundTrip ("empty", s, expectedOf ("", "", "", "", ""));
  }

  // 5. bufferValid lifecycle + untouched buffer before first encode.
  {
    serializer s;
    CHECK (!s.bufferValid ());
    CHECK (s.buffer ().empty ());
    CHECK (s.encode () > 0);
    CHECK (s.bufferValid ());
    CHECK (!s.buffer ().empty ());
  }

  // 6. Unknown tags are skipped by length (forward compatibility): build a
  //    payload with a foreign tag injected between known ones, frame it
  //    with the reference encoder and parse it with the reference parser.
  {
    std::vector<uint8_t> payload;
    auto                 field = [&payload] (fieldTag tag, const std::string &v)
      {
        payload.push_back (static_cast<uint8_t> (tag));
        payload.push_back (static_cast<uint8_t> (v.size ()));
        payload.push_back (0);
        payload.insert (payload.end (), v.begin (), v.end ());
      };
    field (fieldTag::title, "Known");
    field (static_cast<fieldTag> (0x7F), "XXXX"); // unknown tag, 4 bytes
    field (fieldTag::playbackStatus, "Playing");

    // Frame it with the full new wire layout: CRC (reference-computed) +
    // COBS block, wrapped in the 0x55 0xAA preamble, the len:3 field and
    // the 0x00 delimiter.
    const uint16_t crc = crc16Ref (payload);
    payload.push_back (static_cast<uint8_t> (crc & 0xFF));
    payload.push_back (static_cast<uint8_t> (crc >> 8));

    std::vector<uint8_t> frame (4096);
    frame[0] = 0x55;
    frame[1] = 0xAA;
    const size_t frameLen = cobsRefEncode (payload.data (), payload.size (), frame.data () + 5);
    frame[2] = static_cast<uint8_t> (frameLen & 0xFF); // len:3 LE = COBS block size
    frame[3] = static_cast<uint8_t> ((frameLen >> 8) & 0xFF);
    frame[4] = static_cast<uint8_t> ((frameLen >> 16) & 0xFF);
    frame.resize (5 + frameLen);
    frame.push_back (0x00);

    const ParsedTlv parsed = parseTlv (decodeFrame (frame));
    CHECK (parsed.fields.size () == 2);
    CHECK (parsed.fields.at (fieldTag::title) == "Known");
    CHECK (parsed.fields.at (fieldTag::playbackStatus) == "Playing");
    CHECK (parsed.skippedUnknownBytes == 3 + 4); // header + value skipped
  }

  // 6b. Known-answer check for the reference CRC itself: CRC-16/CCITT-FALSE
  //     of the canonical test string "123456789" must be 0x29B1.
  {
    const std::vector<uint8_t> check = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    CHECK (crc16Ref (check) == 0x29B1);
  }

  // 6c. Corruption detection: flip one bit inside a real frame; the decoded
  //     payload's CRC must no longer verify (this is what the firmware
  //     decoder relies on to reject a frame).
  {
    serializer s;
    s.setTitle ("Never Gonna Give You Up");
    CHECK (s.encode () > 0);
    std::vector<uint8_t> corrupted = s.buffer ();
    corrupted[corrupted.size () / 2] ^= 0x01; // flip 1 bit mid-frame

    // Manual receive path: preamble + length + delimiter, COBS-decode,
    // CRC check.
    CHECK (corrupted[0] == 0x55 && corrupted[1] == 0xAA && corrupted.back () == 0x00);
    std::vector<uint8_t> decoded = cobsDecode ({ corrupted.begin () + 5, corrupted.end () - 1 });
    CHECK (decoded.size () >= 2);
    const uint16_t stored
        = static_cast<uint16_t> (decoded[decoded.size () - 2] | (decoded[decoded.size () - 1] << 8));
    decoded.resize (decoded.size () - 2);
    CHECK (crc16Ref (decoded) != stored); // mismatch -> frame rejected
  }

  // 7. Protocol ceiling: 2-byte length clamps at 65535, never wraps.
  {
    const std::string huge (70000, 'x');
    serializer        s;
    s.setTitle (huge);
    const size_t n = s.encode ();
    CHECK (n > 0xFFFF); // the frame really carries the huge field
    // Third length byte is exercised: the COBS block exceeds 64 KB, so a
    // 2-byte length would have overflowed here (why the field is 3 bytes).
    CHECK (s.buffer ()[4] != 0);
    const ParsedTlv parsed = parseTlv (decodeFrame (s.buffer ()));
    CHECK (parsed.fields.at (fieldTag::title).size () == 0xFFFF);
    CHECK (parsed.fields.at (fieldTag::title).front () == 'x');
    CHECK (parsed.fields.at (fieldTag::title).back () == 'x');
  }

  // 8. Position field: fixed 8-byte value, uint32 LE pos_ms + dur_ms, in
  //    every metadata frame alongside the text tags.
  {
    serializer s;
    s.setApp ("App");
    s.setPosition (0x01020304, 0x0A0B0C0D);
    roundTrip ("position", s, expectedOf ("App", "", "", "", ""));
    const ParsedTlv parsed = parseTlv (decodeFrame (s.buffer ()));
    CHECK (parsed.binaryFields.count (fieldTag::position) == 1);
    const auto &v = parsed.binaryFields.at (fieldTag::position);
    CHECK (v.size () == 8);
    CHECK (v[0] == 0x04 && v[1] == 0x03 && v[2] == 0x02 && v[3] == 0x01); // little-endian
    CHECK (v[4] == 0x0D && v[5] == 0x0C && v[6] == 0x0B && v[7] == 0x0A);
  }

  // 9. Position sentinel: untouched serializer reports unknown (0xFFFFFFFF),
  //    and values full of zero bytes survive COBS (block-close paths).
  {
    serializer s;
    s.setPosition (0x00000000, 0xFFFFFFFF);
    CHECK (s.encode () > 0);
    const ParsedTlv parsed = parseTlv (decodeFrame (s.buffer ()));
    const auto     &v = parsed.binaryFields.at (fieldTag::position);
    CHECK (v.size () == 8);
    CHECK (v[0] == 0x00 && v[1] == 0x00 && v[2] == 0x00 && v[3] == 0x00);
    CHECK (v[4] == 0xFF && v[5] == 0xFF && v[6] == 0xFF && v[7] == 0xFF);
  }

  // 10. Art frame: raw [0x00 0x00 marker][w:2 LE][h:2 LE][RGB565 BE
  //     pixels] - no COBS/TLV/CRC. The payload carries 0x00 bytes (black
  //     pixels) to pin that raw zeros pass through untouched. 500 bytes =
  //     25x10 pixels at 2 bytes each.
  {
    std::vector<uint8_t> art;
    for (size_t i = 0; i < 500; ++i)
      art.push_back (static_cast<uint8_t> ((i % 3 == 0) ? 0x00 : (i & 0xFF)));

    serializer s;
    s.setThumbnail (art, 25, 10);
    const size_t n = s.encodeThumbnailFrame ();
    CHECK (n > 0);
    CHECK (s.bufferValid ());
    CHECK (s.buffer ().size () == n);
    CHECK (n == art.size () + 4 + 8); // pixels + w/h header + framing (2+3+2+1)

    const ArtFrame parsed = decodeArtFrame (s.buffer ());
    CHECK (parsed.width == 25 && parsed.height == 10);
    CHECK (parsed.pixels == art); // pixels opaque (BE) and intact

    // The exposed crc16 helper (used by the console CRC printout in
    // main.cpp) must agree with the independent reference CRC.
    CHECK (serializer::crc16 (art.data (), art.size ()) == crc16Ref (art));

    // Data frames must NOT pick up the stored art (it is not a TLV field
    // anymore); the position field is there as usual.
    s.setApp ("X");
    CHECK (s.encode () > 0);
    const ParsedTlv meta = parseTlv (decodeFrame (s.buffer ()));
    CHECK (meta.fields == expectedOf ("X", "", "", "", ""));
    CHECK (meta.binaryFields.count (fieldTag::position) == 1);
  }

  // 11. Art validation: the w/h header must agree with the pixel count -
  //     refuse instead of clamp (a truncated image would misframe the
  //     blit). The old 65535 TLV ceiling died with tag 7: the raw art
  //     frame is limited only by the 3-byte length field.
  {
    serializer s;

    // Pixel count != width*height*2 -> refused.
    std::vector<uint8_t> wrongSize (100, 0xAB);
    s.setThumbnail (wrongSize, 8, 8);
    CHECK (s.encodeThumbnailFrame () == 0);
    CHECK (!s.bufferValid ());
    CHECK (s.buffer ().empty ());

    // 182x182 = 66248 pixel bytes: over the old 65535 ceiling, fine for
    // the raw art frame -> accepted and round-trips.
    std::vector<uint8_t> big (182u * 182u * 2u, 0xCD);
    s.setThumbnail (big, 182, 182);
    CHECK (s.encodeThumbnailFrame () > 0);
    const ArtFrame parsed = decodeArtFrame (s.buffer ());
    CHECK (parsed.width == 182 && parsed.height == 182);
    CHECK (parsed.pixels == big);
    CHECK (s.buffer ()[4] != 0); // len:3 really uses its third byte here
  }

  // 12. Empty art: encodeThumbnailFrame refuses (0, invalid buffer).
  {
    serializer s;
    CHECK (s.encodeThumbnailFrame () == 0);
    CHECK (!s.bufferValid ());
    CHECK (s.buffer ().empty ());
  }

  // 13. Thumbnail round-trip through a full encode()/parse cycle once more
  //     after the binary-heavy tests, to be sure state carries over cleanly.
  {
    serializer s;
    s.setPlaybackStatus ("Paused");
    s.setPosition (1000, 2000);
    roundTrip ("after-thumb", s, expectedOf ("", "", "", "", "Paused"));
  }


  // 14. Cross-check against the firmware decoder: the same six cases the
  //     reference decoders above cover, decoded by Soft/user_app/src/protocol.c.
  {
    serializer s;
    s.setApp ("Spotify");
    s.setTitle ("Never Gonna Give You Up");
    s.setArtist ("Rick Astley");
    s.setAlbum ("Whenever You Need Somebody");
    s.setPlaybackStatus ("Playing");
    firmwareRoundTrip ("short", s, "Spotify", "Never Gonna Give You Up", "Rick Astley", "Whenever You Need Somebody",
                       "Playing", 0u, 0xFFFFFFFFu);
  }
  {
    const std::string longTitle (400, 'a');
    const std::string longAlbum (300, 'b');
    serializer        s;
    s.setTitle (longTitle);
    s.setAlbum (longAlbum);
    // The firmware truncates over-long fields to PROTOCOL_TEXT_MAX-1 bytes; a
    // 400-byte title is beyond that, so compare the decoder's prefix only.
    CHECK (s.encode () > 0);
    const protocolFields_t fields = decodeWithFirmwareDecoder (s.buffer ());
    CHECK (strlen (fields.title) == PROTOCOL_TEXT_MAX - 1u);
    CHECK (strlen (fields.album) == PROTOCOL_TEXT_MAX - 1u);
    // Text prefix survives; the tail is the decoder's UTF-8 ellipsis.
    CHECK (strncmp (fields.title, longTitle.c_str (), PROTOCOL_TEXT_MAX - 4u) == 0);
    CHECK (strncmp (fields.album, longAlbum.c_str (), PROTOCOL_TEXT_MAX - 4u) == 0);
    CHECK (static_cast<uint8_t> (fields.title[strlen (fields.title) - 3u]) == 0xE2u);
    CHECK (static_cast<uint8_t> (fields.title[strlen (fields.title) - 2u]) == 0x80u);
    CHECK (static_cast<uint8_t> (fields.title[strlen (fields.title) - 1u]) == 0xA6u);
    printf ("[firmware long] frame=%zu ok\n", s.buffer ().size ());
  }
  {
    const std::string artist = "Bj\xc3\xb6rk Gu\xc3\xb0mundsd\xc3\xb3ttir \xF0\x9F\x8E\xB5";
    serializer        s;
    s.setArtist (artist);
    firmwareRoundTrip ("utf8", s, "", "", artist.c_str (), "", "", 0u, 0xFFFFFFFFu);
  }
  {
    serializer s;
    firmwareRoundTrip ("empty", s, "", "", "", "", "", 0u, 0xFFFFFFFFu);
  }
  {
    serializer s;
    s.setApp ("App");
    firmwareRoundTrip ("position", s, "App", "", "", "", "", 0x01020304u, 0x0A0B0C0Du);
  }
  {
    serializer s;
    s.setPlaybackStatus ("Paused");
    firmwareRoundTrip ("after-thumb", s, "", "", "", "", "Paused", 1000u, 2000u);
  }

  // 14b. Art frames carry no CRC, so the firmware validates them by length and
  //      dimensions. A single flipped pixel leaves the length intact and must
  //      still be accepted (the plan's bit-flip check), while a wrong length
  //      or an out-of-panel size must be rejected.
  {
    std::vector<uint8_t> art (160u * 88u * 2u, 0x5A);
    serializer           s;
    s.setThumbnail (art, 160, 88);
    CHECK (s.encodeThumbnailFrame () > 0);
    const std::vector<uint8_t> &frame = s.buffer ();
    const uint32_t artLen
        = static_cast<uint32_t> (frame[2]) | (static_cast<uint32_t> (frame[3]) << 8) | (static_cast<uint32_t> (frame[4]) << 16);
    const uint8_t              *body = frame.data () + 5;
    const uint16_t width = static_cast<uint16_t> (body[2] | (body[3] << 8));
    const uint16_t height = static_cast<uint16_t> (body[4] | (body[5] << 8));
    CHECK (width == 160u && height == 88u);
    CHECK (protocolValidateArtFrame (artLen, width, height, 160u, 160u));

    std::vector<uint8_t> flipped = frame;
    flipped[frame.size () / 2u] ^= 0x01u; // one pixel bit
    CHECK (protocolValidateArtFrame (static_cast<uint32_t> (artLen), width, height, 160u, 160u));

    CHECK (!protocolValidateArtFrame (artLen - 1u, width, height, 160u, 160u));
    CHECK (!protocolValidateArtFrame (artLen, 0u, 88u, 160u, 160u));
    CHECK (!protocolValidateArtFrame (artLen, 161u, 88u, 160u, 160u));
  }

  }
  catch (const std::exception &e)
    {
      printf ("\nUNCAUGHT std::exception: %s\n", e.what ());
      ++failures;
    }
  catch (...)
    {
      printf ("\nUNCAUGHT unknown (non-std) exception\n");
      ++failures;
    }

  if (failures)
    printf ("\nFAILED: %d check(s)\n", failures);
  else
    printf ("\nAll checks passed.\n");
  return failures ? 1 : 0;
}
