// PFRM -- the wire format for a pre-rendered panel image.
//
// This header is THE CONTRACT between the firmware and whatever produces images
// (tools/encode_image.py today, the photoframe-webhook service later). If you change
// anything here, change it in tools/encode_image.py in the same commit and bump
// PFRM_VERSION.
//
// Why a pre-packed blob at all: the ESP32-S3 has no business decoding JPEG, resizing,
// and dithering 1.9 megapixels on battery. The server does all of that and ships bytes
// that go straight into the Seeed_GFX framebuffer with a memcpy.
//
// Layout:   [ 64-byte PfrmHeader ][ data_len bytes of packed pixels ]
// Total for the 13.3" panel: 64 + 960000 = 960064 bytes.
//
// Pixels:   4 bits per pixel, 2 pixels per byte, row-major, top-left origin.
//           HIGH nibble is the even-x (left) pixel, LOW nibble the odd-x (right).
//           Stride is width/2 = 600 bytes; there is no row padding (1200 is even).
//
// Endianness: little-endian throughout. Both ends are LE, so nothing is swapped
//             anywhere. Do not "helpfully" add htonl().
#pragma once

#include <stdint.h>

#define PFRM_MAGIC   0x4D524650u  // 'P','F','R','M' read back as a LE uint32
#define PFRM_VERSION 1

// format field
#define PFRM_FMT_SEEED_GFX_4BPP 1

// palette_id field
#define PFRM_PALETTE_SPECTRA6 1

// Spectra 6 colours, in the encoding Seeed_GFX's *framebuffer* uses.
//
// These are NOT the codes the panel hardware wants. Seeed_GFX's COLOR_GET macro
// (TFT_Drivers/T133A01_Defines.h) translates buffer -> panel during the SPI push:
//
//     buffer 0x0 white  -> panel 0x1        buffer 0x2 green  -> panel 0x6
//     buffer 0x6 red    -> panel 0x3        buffer 0xB yellow -> panel 0x2
//     buffer 0xD blue   -> panel 0x5        buffer 0xF black  -> panel 0x0
//
// We pack the buffer encoding so the blob can be memcpy'd into the framebuffer with
// zero translation on the MCU. Anything outside this set falls through COLOR_GET to
// white, so an encoder bug shows up as a washed-out image rather than noise.
//
// Note the pleasant accident: an all-white frame is all 0x00 bytes, which is also
// what calloc gives you.
enum PfrmColor : uint8_t {
  PFRM_WHITE  = 0x0,
  PFRM_GREEN  = 0x2,
  PFRM_RED    = 0x6,
  PFRM_YELLOW = 0xB,
  PFRM_BLUE   = 0xD,
  PFRM_BLACK  = 0xF,
};

#pragma pack(push, 1)
struct PfrmHeader {
  uint32_t magic;         // 0x00  PFRM_MAGIC
  uint16_t version;       // 0x04  PFRM_VERSION
  uint16_t header_len;    // 0x06  64
  uint16_t width;         // 0x08  1200
  uint16_t height;        // 0x0A  1600
  uint8_t  format;        // 0x0C  PFRM_FMT_*
  uint8_t  rotation;      // 0x0D  quarter-turns the firmware should apply (normally 0)
  uint16_t flags;         // 0x0E  bit0 = compressed (reserved, always 0 for now)
  uint32_t data_len;      // 0x10  960000
  uint32_t data_crc32;    // 0x14  CRC-32/ISO-HDLC over the pixel bytes only
  uint64_t created_at;    // 0x18  unix epoch seconds, informational
  uint32_t palette_id;    // 0x20  PFRM_PALETTE_*
  uint32_t header_crc32;  // 0x24  CRC-32 over bytes 0x00..0x23. Lets a bad transfer
                          //       abort after 64 bytes instead of after 960 KB.
  uint8_t  reserved[24];  // 0x28  zeroed
};
#pragma pack(pop)

#ifdef __cplusplus
static_assert(sizeof(PfrmHeader) == 64, "PFRM header must be exactly 64 bytes");
#endif

#define PFRM_HEADER_LEN 64
