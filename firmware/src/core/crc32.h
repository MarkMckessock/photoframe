// CRC-32/ISO-HDLC, the one zlib computes -- and the reason this file exists.
//
// esp_rom_crc32_le() is NOT that function. The ROM routine omits the pre- and
// post-inversion, so esp_rom_crc32_le(0, buf, len) disagrees with
// zlib.crc32(buf) for every input. Calling it directly would mean every image
// ever produced by tools/encode_image.py failed its CRC check and the panel
// silently never updated -- a fault that looks like a network problem and is
// miserable to chase on hardware.
//
// esp_rom_crc.h documents the convention: invert going in, invert coming out.
// It also chains, which is what lets the HTTP path fold each arriving chunk in
// as it lands instead of making a second pass over a megabyte.
//
//     uint32_t crc = 0;
//     crc = pf_crc32(crc, chunk0, n0);
//     crc = pf_crc32(crc, chunk1, n1);   // == zlib.crc32(chunk0 + chunk1)
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <esp_rom_crc.h>

static inline uint32_t pf_crc32(uint32_t crc, const uint8_t* data, size_t len) {
  return ~esp_rom_crc32_le(~crc, data, len);
}
