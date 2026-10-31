/* Host-side tests for the firmware's COBS/CRC/TLV decoder.
   Build with: cc -std=c11 -Wall -Wextra -Werror -I../Soft/user_app/inc \
     tests/test_protocol.c ../Soft/user_app/src/protocol.c -o test_protocol */
#include "protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned failures;
#define CHECK(expr)                                                                                                    \
  do                                                                                                                   \
    {                                                                                                                  \
      if (!(expr))                                                                                                     \
        {                                                                                                              \
          (void)fprintf (stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);                                       \
          failures++;                                                                                                  \
        }                                                                                                              \
    }                                                                                                                  \
  while (0)

static size_t appendCrcAndCobs (const uint8_t *tlv_ptr, size_t tlvLen, uint8_t *out_ptr, size_t outCap)
{
  uint8_t raw[DL_PAYLOAD_MAX];
  if ((tlvLen + 2u) > sizeof (raw))
    {
      return 0u;
    }
  memcpy (raw, tlv_ptr, tlvLen);
  const uint16_t crc = protocolCrc16 (tlv_ptr, tlvLen);
  raw[tlvLen] = (uint8_t)crc;
  raw[tlvLen + 1u] = (uint8_t)(crc >> 8);
  const size_t n = tlvLen + 2u;

  size_t codeIdx = 0u;
  size_t outIdx = 1u;
  uint8_t code = 1u;
  if (outCap == 0u)
    {
      return 0u;
    }
  for (size_t i = 0; i < n; i++)
    {
      if (raw[i] == 0u)
        {
          out_ptr[codeIdx] = code;
          if (outIdx >= outCap)
            {
              return 0u;
            }
          codeIdx = outIdx++;
          code = 1u;
        }
      else
        {
          if (outIdx >= outCap)
            {
              return 0u;
            }
          out_ptr[outIdx++] = raw[i];
          code++;
          if (code == 0xFFu)
            {
              out_ptr[codeIdx] = code;
              if (i + 1u < n)
                {
                  if (outIdx >= outCap)
                    {
                      return 0u;
                    }
                  codeIdx = outIdx++;
                  code = 1u;
                }
            }
        }
    }
  if (codeIdx == outIdx)
    {
      if (outIdx >= outCap)
        {
          return 0u;
        }
      out_ptr[outIdx++] = code;
    }
  else
    {
      out_ptr[codeIdx] = code;
    }
  return outIdx;
}

static void testKnownPositionVector (void)
{
  /* COBS block from the parser review; decoded pos=32713 ms, dur=539037 ms,
     CRC=0xC224. */
  const uint8_t block[] = { 0x03, 0x06, 0x08, 0x03, 0xC9, 0x7F, 0x01, 0x04, 0x9D, 0x3D, 0x08, 0x03, 0x24, 0xC2 };
  protocolFields_t fields;
  CHECK (protocolCrc16 ((const uint8_t *)"123456789", 9u) == 0x29B1u);
  CHECK (protocolDecodeDataFrame (block, sizeof (block), &fields) == PR_OK_u8);
  CHECK ((fields.presentMask & PROTO_HAS_POSITION) != 0u);
  CHECK (fields.positionMs == 32713u);
  CHECK (fields.durationMs == 540061u);
}

static void testMetadataRoundTrip (void)
{
  const uint8_t tlv[] = {
    0x01, 0x03, 0x00, 'A', 'p', 'p',
    0x02, 0x05, 0x00, 'T', 'i', 't', 'l', 'e',
    0x03, 0x04, 0x00, 'N', 'a', 'm', 'e',
    0x04, 0x00, 0x00,
    0x05, 0x07, 0x00, 'P', 'l', 'a', 'y', 'i', 'n', 'g',
  };
  uint8_t block[128];
  const size_t blockLen = appendCrcAndCobs (tlv, sizeof (tlv), block, sizeof (block));
  protocolFields_t fields;
  CHECK (blockLen > 0u);
  CHECK (protocolDecodeDataFrame (block, (uint32_t)blockLen, &fields) == PR_OK_u8);
  CHECK (strcmp (fields.app, "App") == 0);
  CHECK (strcmp (fields.title, "Title") == 0);
  CHECK (strcmp (fields.artist, "Name") == 0);
  CHECK (strcmp (fields.album, "") == 0);
  CHECK (strcmp (fields.playbackStatus, "Playing") == 0);
  CHECK ((fields.presentMask & (PROTO_HAS_APP | PROTO_HAS_TITLE | PROTO_HAS_ARTIST | PROTO_HAS_ALBUM | PROTO_HAS_STATUS))
         == (PROTO_HAS_APP | PROTO_HAS_TITLE | PROTO_HAS_ARTIST | PROTO_HAS_ALBUM | PROTO_HAS_STATUS));
}

static void testUnknownTagSkipped (void)
{
  const uint8_t tlv[] = { 0x7F, 0x03, 0x00, 0x11, 0x22, 0x33, 0x02, 0x01, 0x00, 'x' };
  uint8_t block[64];
  const size_t blockLen = appendCrcAndCobs (tlv, sizeof (tlv), block, sizeof (block));
  protocolFields_t fields;
  CHECK (protocolDecodeDataFrame (block, (uint32_t)blockLen, &fields) == PR_OK_u8);
  CHECK (strcmp (fields.title, "x") == 0);
}

static void testArtValidation (void)
{
  CHECK (protocolValidateArtFrame (4u + 160u * 88u * 2u, 160u, 88u, 160u, 160u));
  CHECK (!protocolValidateArtFrame (4u + 160u * 88u * 2u - 1u, 160u, 88u, 160u, 160u));
  CHECK (!protocolValidateArtFrame (4u, 0u, 1u, 160u, 160u));
  CHECK (!protocolValidateArtFrame (4u + 161u * 1u * 2u, 161u, 1u, 160u, 160u));
  CHECK (!protocolValidateArtFrame (4u + 160u * 161u * 2u, 160u, 161u, 160u, 160u));
}

static void testRejectedFrames (void)
{
  protocolFields_t fields;
  const uint8_t badCobs[] = { 0x04, 0x01 }; /* claims three following bytes */
  CHECK (protocolDecodeDataFrame (badCobs, sizeof (badCobs), &fields) == PR_COBS_ERR_u8);

  const uint8_t tlv[] = { 0x02, 0x01, 0x00, 'x' };
  uint8_t block[64];
  size_t blockLen = appendCrcAndCobs (tlv, sizeof (tlv), block, sizeof (block));
  CHECK (blockLen > 0u);
  block[blockLen - 1u] ^= 0x01u;
  CHECK (protocolDecodeDataFrame (block, (uint32_t)blockLen, &fields) == PR_CRC_ERR_u8);

  const uint8_t truncatedTlv[] = { 0x02, 0x02, 0x00, 'x' }; /* length says 2, only 1 value byte */
  blockLen = appendCrcAndCobs (truncatedTlv, sizeof (truncatedTlv), block, sizeof (block));
  CHECK (protocolDecodeDataFrame (block, (uint32_t)blockLen, &fields) == PR_TLV_ERR_u8);

  CHECK (protocolDecodeDataFrame (block, DL_PAYLOAD_MAX + 1u, &fields) == PR_COBS_ERR_u8);
  CHECK (protocolDecodeDataFrame (NULL, 1u, &fields) == PR_COBS_ERR_u8);
}

static void testLongUtf8TruncatesAtBoundary (void)
{
  uint8_t tlv[3u + 400u];
  tlv[0] = 0x02;
  tlv[1] = 0x90u;
  tlv[2] = 0x01u;
  memset (&tlv[3], 'a', 390u);
  tlv[3u + 390u] = 0xE2u;
  tlv[3u + 391u] = 0x82u;
  tlv[3u + 392u] = 0xACu;
  memset (&tlv[3u + 393u], 'z', 7u);
  uint8_t block[512];
  const size_t blockLen = appendCrcAndCobs (tlv, sizeof (tlv), block, sizeof (block));
  protocolFields_t fields;
  CHECK (protocolDecodeDataFrame (block, (uint32_t)blockLen, &fields) == PR_OK_u8);
  CHECK (strlen (fields.title) <= PROTOCOL_TEXT_MAX - 1u);
  const size_t titleLen = strlen (fields.title);
  CHECK ((titleLen >= 3u) && ((uint8_t)fields.title[titleLen - 3u] == 0xE2u) && ((uint8_t)fields.title[titleLen - 2u] == 0x80u)
         && ((uint8_t)fields.title[titleLen - 1u] == 0xA6u));
}

int main (void)
{
  testKnownPositionVector ();
  testMetadataRoundTrip ();
  testUnknownTagSkipped ();
  testArtValidation ();
  testRejectedFrames ();
  testLongUtf8TruncatesAtBoundary ();
  if (failures != 0u)
    {
      (void)fprintf (stderr, "%u checks failed\n", failures);
      return EXIT_FAILURE;
    }
  (void)puts ("All protocol checks passed.");
  return EXIT_SUCCESS;
}
