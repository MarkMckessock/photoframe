// A copy of the last good image in flash (LittleFS on the `storage` partition).
//
// This is not a performance cache -- fetching over WiFi is faster than reading a
// megabyte out of SPI flash. It exists so that the firmware can put *something
// correct* on the panel in the two situations where it must draw but cannot download:
//
//   - we browned out mid-refresh, so the panel is genuinely showing garbage;
//   - the battery is nearly flat and we need to say so, and the only alternative to
//     compositing a warning onto the cached photo is erasing the photo to make room.
//
// Both are rare, and both are the difference between a frame that degrades gracefully
// and one that goes blank on you.
#pragma once

#include <stdint.h>
#include <stddef.h>

namespace pf {

class ImageBuffer;

namespace cache {

bool begin();
bool exists();

// Writes to a temp path and renames, so an interrupted save cannot leave a
// half-written file that later passes a size check and fails a CRC.
bool save(const uint8_t* blob, size_t len);

// Reads into `buf` and validates it. A cache that fails validation is deleted --
// there is no point keeping a file we will never be willing to draw.
bool load(ImageBuffer& buf);

}  // namespace cache
}  // namespace pf
