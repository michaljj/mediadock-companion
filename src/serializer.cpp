#include "serializer.hpp"

#include <algorithm>
#include <array>

// ---------------------------------------------------------------------------
// encode(): TLV assembly + COBS + framing
// ---------------------------------------------------------------------------

// One TLV field: [tag:1][len:2 little-endian][value]. The value carries no
// NUL terminator - the length delimits it, so any string length fits.
void serializer::appendField (std::vector<uint8_t> &payload, fieldTag tag, const std::string &value)
{
  appendRawField (payload, tag, reinterpret_cast<const uint8_t *> (value.data ()), value.size ());
}

// Binary-value variant (position, thumbnail). Same clamping rules as the
// text path: the 2-byte length field is the only ceiling, clamp, never wrap.
void serializer::appendRawField (std::vector<uint8_t> &payload, fieldTag tag, const uint8_t *value, size_t len)
{
  const size_t clamped = (std::min)(len, size_t{ 0xFFFF });

  payload.push_back (static_cast<uint8_t> (tag));
  payload.push_back (static_cast<uint8_t> (clamped & 0xFF));
  payload.push_back (static_cast<uint8_t> (clamped >> 8));
  payload.insert (payload.end (), value, value + static_cast<std::ptrdiff_t> (clamped));
}

// COBS-encode the finished payload and append the framing. Shared tail of
// encode() and encodePositionFrame() (encodeThumbnailFrame() builds the
// raw art frame and does not COBS-encode anything):
//   1. append CRC-16 over the TLV bytes (low byte first), INSIDE the COBS
//      block - the receiver decodes first, then verifies, then walks TLV
//   2. prepend the 0x55 0xAA preamble (fast startup resync, see header)
//   3. COBS-encode, patch the 3-byte length (COBS block byte count, LE),
//      append the 0x00 delimiter
// The length rides OUTSIDE the COBS block as raw bytes (it may legitimately
// contain 0x00): after the preamble, framing is length-based, so the bytes
// cannot be mistaken for a delimiter. Its whole point is that a
// memory-limited receiver can size or reject the frame BEFORE storing it.
void serializer::finishFrame (std::vector<uint8_t> &payload)
{
  const uint16_t crc = crc16 (payload.data (), payload.size ());
  payload.push_back (static_cast<uint8_t> (crc & 0xFF));
  payload.push_back (static_cast<uint8_t> (crc >> 8));

  outBuffer_.clear ();
  // Framing overhead: 2 preamble + 3 length + 1 delimiter on top of the
  // COBS worst case (n + n/254 + 1).
  outBuffer_.reserve (payload.size () + ((payload.size () + 254) / 254) + 6);
  outBuffer_.push_back (0x55); // preamble
  outBuffer_.push_back (0xAA);
  const size_t lenIdx = outBuffer_.size ();
  outBuffer_.push_back (0); // length placeholder, patched after cobsAppend
  outBuffer_.push_back (0);
  outBuffer_.push_back (0);
  cobsAppend (payload.data (), payload.size (), outBuffer_);

  // Patch len:3 LE with the COBS block size. Placeholder-patch, not
  // insert: same rules as the COBS code bytes (indices only).
  const size_t cobsLen = outBuffer_.size () - lenIdx - 3;
  outBuffer_[lenIdx] = static_cast<uint8_t> (cobsLen & 0xFF);
  outBuffer_[lenIdx + 1] = static_cast<uint8_t> ((cobsLen >> 8) & 0xFF);
  outBuffer_[lenIdx + 2] = static_cast<uint8_t> ((cobsLen >> 16) & 0xFF);

  outBuffer_.push_back (0x00); // frame delimiter: receiver re-syncs on it
  bufferValid_ = true;
}

// CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflection, no final
// XOR. Known answer: crc16("123456789") == 0x29B1 (pinned by the tests).
// Bitwise, no lookup table: at the ~66 KB protocol ceiling that is ~520 K
// inner steps worst case - trivial for the PC, and the dock verifies one
// frame at a time. Zero table RAM on the MCU in exchange.
uint16_t serializer::crc16 (const uint8_t *data, size_t n)
{
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < n; ++i)
    {
      crc ^= static_cast<uint16_t> (data[i]) << 8;
      for (int bit = 0; bit < 8; ++bit)
        {
          crc = (crc & 0x8000) ? static_cast<uint16_t> ((crc << 1) ^ 0x1021) : static_cast<uint16_t> (crc << 1);
        }
    }
  return crc;
}

size_t serializer::encode ()
{
  // 1. Assemble the TLV payload (fixed tag order, see fieldTag).
  std::vector<uint8_t> payload;
  payload.reserve (app_.size () + title_.size () + artist_.size () + album_.size () + playbackStatus_.size ()
                   + (size_t{ 5 } * 3) + 3 + 8); // + position header + fixed 8-byte value
  appendField (payload, fieldTag::app, app_);
  appendField (payload, fieldTag::title, title_);
  appendField (payload, fieldTag::artist, artist_);
  appendField (payload, fieldTag::album, album_);
  appendField (payload, fieldTag::playbackStatus, playbackStatus_);

  // Position travels in every metadata frame so the dock can re-anchor its
  // 1 Hz interpolation whenever a real event arrives.
  std::array<uint8_t, 8> position{};
  position[0] = static_cast<uint8_t> (positionMs_ & 0xFF);
  position[1] = static_cast<uint8_t> ((positionMs_ >> 8) & 0xFF);
  position[2] = static_cast<uint8_t> ((positionMs_ >> 16) & 0xFF);
  position[3] = static_cast<uint8_t> ((positionMs_ >> 24) & 0xFF);
  position[4] = static_cast<uint8_t> (durationMs_ & 0xFF);
  position[5] = static_cast<uint8_t> ((durationMs_ >> 8) & 0xFF);
  position[6] = static_cast<uint8_t> ((durationMs_ >> 16) & 0xFF);
  position[7] = static_cast<uint8_t> ((durationMs_ >> 24) & 0xFF);
  appendRawField (payload, fieldTag::position, position.data (), position.size ());

  // 2. COBS-encode into the output buffer and frame it.
  finishFrame (payload);

  return outBuffer_.size ();
}

size_t serializer::encodePositionFrame ()
{
  std::array<uint8_t, 8> position{};
  position[0] = static_cast<uint8_t> (positionMs_ & 0xFF);
  position[1] = static_cast<uint8_t> ((positionMs_ >> 8) & 0xFF);
  position[2] = static_cast<uint8_t> ((positionMs_ >> 16) & 0xFF);
  position[3] = static_cast<uint8_t> ((positionMs_ >> 24) & 0xFF);
  position[4] = static_cast<uint8_t> (durationMs_ & 0xFF);
  position[5] = static_cast<uint8_t> ((durationMs_ >> 8) & 0xFF);
  position[6] = static_cast<uint8_t> ((durationMs_ >> 16) & 0xFF);
  position[7] = static_cast<uint8_t> ((durationMs_ >> 24) & 0xFF);

  std::vector<uint8_t> payload;
  payload.reserve (3 + position.size ());
  appendRawField (payload, fieldTag::position, position.data (), position.size ());

  finishFrame (payload);

  return outBuffer_.size ();
}

size_t serializer::encodeThumbnailFrame ()
{
  // Raw art frame: [55 AA][len:3][0x00 0x00 kind marker][width:2 LE]
  // [height:2 LE][pixels][0x00 delimiter]. No COBS/TLV/CRC: the pixels
  // are raw so the dock can shift them to the panel with zero decode work.
  // len:3 counts the w/h header + pixels (4 + width*height*2); the 0x00
  // 0x00 marker right after the length is what identifies the frame (NOT
  // the length - art size varies with the aspect ratio). The header must
  // agree with the pixel count, so refuse rather than clamp: a truncated
  // image with a full-size header would misframe the dock's blit. main.cpp
  // scales art to fit 160x160, far below the only ceiling here (the
  // 3-byte length reaches 16 MB).
  const size_t expected = static_cast<size_t> (thumbWidth_) * thumbHeight_ * 2;
  if (thumbnail_.empty () || thumbWidth_ == 0 || thumbHeight_ == 0 || thumbnail_.size () != expected
      || expected + 4 > 0xFFFFFF)
    {
      // Nothing valid to send: invalidate the buffer so callers can't
      // write stale art.
      outBuffer_.clear ();
      bufferValid_ = false;
      return 0;
    }

  outBuffer_.clear ();
  // Framing overhead: 2 preamble + 3 length + 2 marker + 1 delimiter on
  // top of the 4-byte w/h header and the raw pixels.
  outBuffer_.reserve (expected + 4 + 8);
  outBuffer_.push_back (0x55); // preamble
  outBuffer_.push_back (0xAA);
  const size_t lenIdx = outBuffer_.size ();
  outBuffer_.push_back (0); // length placeholder, patched below
  outBuffer_.push_back (0);
  outBuffer_.push_back (0);
  outBuffer_.push_back (0x00); // kind marker: 0x00 0x00 = art frame
  outBuffer_.push_back (0x00);
  outBuffer_.push_back (static_cast<uint8_t> (thumbWidth_ & 0xFF));
  outBuffer_.push_back (static_cast<uint8_t> (thumbWidth_ >> 8));
  outBuffer_.push_back (static_cast<uint8_t> (thumbHeight_ & 0xFF));
  outBuffer_.push_back (static_cast<uint8_t> (thumbHeight_ >> 8));
  outBuffer_.insert (outBuffer_.end (), thumbnail_.begin (), thumbnail_.end ());

  // Patch len:3 LE with the body size (w/h header + pixels). Excludes the
  // preamble, the length field itself, the kind marker and the delimiter.
  const size_t bodyLen = expected + 4;
  outBuffer_[lenIdx] = static_cast<uint8_t> (bodyLen & 0xFF);
  outBuffer_[lenIdx + 1] = static_cast<uint8_t> ((bodyLen >> 8) & 0xFF);
  outBuffer_[lenIdx + 2] = static_cast<uint8_t> ((bodyLen >> 16) & 0xFF);

  outBuffer_.push_back (0x00); // frame delimiter: receiver re-syncs on it
  bufferValid_ = true;
  return outBuffer_.size ();
}

// ---------------------------------------------------------------------------
// COBS (Consistent Overhead Byte Stuffing), index-based, O(n).
//
// Data bytes are appended as-is and a running "code" counts the bytes since
// the last block close. When a zero byte is dropped or the code saturates
// at 0xff, the code byte is patched into the placeholder slot that was
// reserved for it (out[codeIdx]) - no insertions, no retained iterators.
// The receiver inverts this to recover the exact payload bytes.
// ---------------------------------------------------------------------------

// Appends COBS(data[0..n)) to out. Works for any n, including 0 (encodes
// to a single 0x01 code byte). Reallocation-safe by construction: only
// indices are retained across push_backs, never iterators.
void serializer::cobsAppend (const uint8_t *data, size_t n, std::vector<uint8_t> &out)
{
  out.push_back (0); // placeholder for the first code byte

  // Prefix-safe: out may already hold the 0x55 0xAA preamble and the
  // 3-byte length placeholder, so the code placeholder sits at
  // out.size()-1, NOT at index 0. Patching index 0 would overwrite the
  // preamble and leave the placeholder as garbage data inside the
  // encoded stream.
  size_t  codeIdx = out.size () - 1;
  uint8_t code = 1;

  for (size_t i = 0; i < n; ++i)
    {
      if (data[i])
        { // Byte not zero, append it
          out.push_back (data[i]);
          ++code;
        }

      if (!data[i] || code == 0xff)
        { // Input is zero or block completed: close the block
          out[codeIdx] = code;
          code = 1;
          codeIdx = out.size ();
          if (!data[i] || i + 1 < n)
            {
              out.push_back (0); // placeholder for the next code byte
            }
        }
    }

  if (codeIdx >= out.size ())
    {
      out.push_back (code); // edge: 0xFF block closing on the last byte
    }
  else
    {
      out[codeIdx] = code;
    }
}
