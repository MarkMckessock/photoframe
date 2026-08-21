// The one big buffer, and everything that decides whether its contents are safe to
// put on the panel.
//
// A full refresh on this panel takes ~30 seconds and cannot be undone -- there is no
// partial update -- so a corrupt blob does not merely fail, it *sticks*, replacing a
// photo of somebody's dog with noise until the next image arrives. That asymmetry is
// why validation here is paranoid and why the header carries its own CRC: a wrong-size
// or wrong-format transfer aborts after 64 bytes instead of after 960 KB.
#pragma once

#include <stdint.h>
#include <stddef.h>

#include "pf_image_format.h"
#include "persist/rtc_state.h"

namespace pf {

class ImageBuffer {
 public:
  // ps_malloc's PF_BLOB_BYTES. Fails if PSRAM is missing or misconfigured, which on
  // this board means memory_type is not qio_opi.
  bool alloc();

  // Start receiving a transfer of exactly `total` bytes.
  bool begin(size_t total);

  // Copy a chunk in. Offsets are expected to arrive in order (they do, over HTTP);
  // out-of-order writes are handled, just more slowly.
  bool write(size_t offset, const uint8_t* data, size_t len);

  // Zero-copy variant: read straight into write_ptr(offset), then commit() what
  // landed. This is how the HTTP client fills the buffer -- a megabyte does not need
  // to be copied twice on its way to the panel.
  uint8_t* write_ptr(size_t offset);
  bool commit(size_t offset, size_t len);

  // Declare that `len` bytes were placed into raw() by some other means (a file
  // read, say). Forces a full CRC pass, since nothing was folded in on the way.
  bool adopt(size_t len);

  bool header_ready() const { return received_ >= PFRM_HEADER_LEN; }
  bool complete() const { return expected_ && received_ == expected_; }

  // Cheap structural checks. Call as soon as header_ready() so a bad transfer dies
  // early rather than after a megabyte.
  bool validate_header();
  // Payload CRC. Call once complete().
  bool validate_payload();
  // Both, for a blob loaded from the cache in one go.
  bool validate_all();

  const PfrmHeader* header() const { return (const PfrmHeader*)buf_; }
  const uint8_t* pixels() const { return buf_ + PFRM_HEADER_LEN; }
  uint8_t* raw() { return buf_; }
  size_t capacity() const { return buf_ ? PF_BLOB_BYTES : 0; }
  size_t received() const { return received_; }

  PfError error() const { return error_; }

 private:
  void fail(PfError e);

  uint8_t* buf_ = nullptr;
  size_t expected_ = 0;
  size_t received_ = 0;
  size_t crc_next_ = 0;   // payload offset the running CRC has consumed up to
  uint32_t crc_ = 0;
  bool crc_dirty_ = false;  // a write arrived out of order; recompute at the end
  bool header_ok_ = false;
  PfError error_ = PF_ERR_NONE;
};

}  // namespace pf
