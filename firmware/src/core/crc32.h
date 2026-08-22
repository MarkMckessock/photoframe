// CRC-32/ISO-HDLC -- the one zlib computes -- and a warning about how to get it wrong.
//
// esp_rom_crc32_le() already IS that function. Despite its name suggesting a bare
// little-endian CRC, the ROM routine inverts internally at both ends:
//
//     esp_rom_crc32_le(init, buf, len)  ==  ~raw_lfsr(~init, buf, len)
//
// Two consequences, both verified against real silicon (see the note below):
//
//   1. esp_rom_crc32_le(0, buf, len) == zlib.crc32(buf). No wrapping required.
//   2. It chains natively from 0, because the inversions cancel across calls:
//        crc = 0;
//        crc = pf_crc32(crc, chunk0, n0);
//        crc = pf_crc32(crc, chunk1, n1);   // == zlib.crc32(chunk0 + chunk1)
//      which is what lets the HTTP path fold each arriving chunk in as it lands
//      instead of making a second pass over a megabyte.
//
// THE TRAP: esp_rom_crc.h documents an `init = ~init; ...; crc = ~crc;` recipe for
// "non-continuous buffers". Applying that here does NOT give you zlib -- it strips the
// ROM's own inversions and yields the raw LFSR value instead. An earlier version of
// this file did exactly that, and the failure was invisible on the host: every image
// was rejected on-device with
//
//     E header crc 0aa0f655 != 60164480
//
// where 0aa0f655 is zlib's answer and 60164480 is raw_lfsr(0, ...). Do not "fix" this
// function by adding tildes. tools/test_format.py pins the convention with that exact
// pair of values.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <esp_rom_crc.h>

static inline uint32_t pf_crc32(uint32_t crc, const uint8_t* data, size_t len) {
  return esp_rom_crc32_le(crc, data, len);
}
