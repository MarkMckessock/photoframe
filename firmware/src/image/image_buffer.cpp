#include "image_buffer.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <string.h>

#include "core/crc32.h"
#include "core/logger.h"
#include "pf_config.h"

namespace pf {

bool ImageBuffer::alloc() {
  if (buf_) return true;
  buf_ = (uint8_t*)heap_caps_malloc(PF_BLOB_BYTES, MALLOC_CAP_SPIRAM);
  if (!buf_) {
    PF_LOGE("no PSRAM for %u byte blob (psram=%u free=%u)", (unsigned)PF_BLOB_BYTES,
            (unsigned)ESP.getPsramSize(), (unsigned)ESP.getFreePsram());
    error_ = PF_ERR_ALLOC;
    return false;
  }
  return true;
}

void ImageBuffer::fail(PfError e) {
  error_ = e;
  expected_ = 0;
  received_ = 0;
}

bool ImageBuffer::begin(size_t total) {
  if (!alloc()) return false;
  if (total < PFRM_HEADER_LEN || total > PF_BLOB_BYTES) {
    PF_LOGE("declared size %u is not plausible (want %u)", (unsigned)total,
            (unsigned)PF_BLOB_BYTES);
    fail(PF_ERR_SIZE);
    return false;
  }
  expected_ = total;
  received_ = 0;
  crc_next_ = 0;
  crc_ = 0;
  crc_dirty_ = false;
  header_ok_ = false;
  error_ = PF_ERR_NONE;
  return true;
}

bool ImageBuffer::adopt(size_t len) {
  if (!buf_ || len > PF_BLOB_BYTES || len < PFRM_HEADER_LEN) {
    fail(PF_ERR_SIZE);
    return false;
  }
  expected_ = len;
  received_ = len;
  crc_dirty_ = true;
  header_ok_ = false;
  error_ = PF_ERR_NONE;
  return true;
}

uint8_t* ImageBuffer::write_ptr(size_t offset) {
  if (!buf_ || !expected_ || offset >= expected_) return nullptr;
  return buf_ + offset;
}

bool ImageBuffer::commit(size_t offset, size_t len) {
  if (!buf_ || !expected_) return false;
  if (offset + len > expected_) {
    PF_LOGE("chunk overruns declared size: %u+%u > %u", (unsigned)offset, (unsigned)len,
            (unsigned)expected_);
    fail(PF_ERR_SIZE);
    return false;
  }
  received_ += len;

  // Fold the payload part of this chunk into the running CRC. Chunks arrive in order
  // over HTTP, so this is normally free; if one ever does not, give up on the
  // incremental path and do a single pass at the end rather than getting it wrong.
  if (!crc_dirty_ && offset + len > PFRM_HEADER_LEN) {
    const size_t payload_start = (offset > PFRM_HEADER_LEN) ? offset : PFRM_HEADER_LEN;
    if (payload_start - PFRM_HEADER_LEN != crc_next_) {
      crc_dirty_ = true;
    } else {
      const size_t n = offset + len - payload_start;
      crc_ = pf_crc32(crc_, buf_ + payload_start, n);
      crc_next_ += n;
    }
  }
  return true;
}

bool ImageBuffer::write(size_t offset, const uint8_t* data, size_t len) {
  uint8_t* dst = write_ptr(offset);
  if (!dst) return false;
  if (dst != data) memcpy(dst, data, len);
  return commit(offset, len);
}

bool ImageBuffer::validate_header() {
  if (!header_ready()) return false;
  const PfrmHeader* h = header();

  if (h->magic != PFRM_MAGIC) {
    PF_LOGE("bad magic %08lx", (unsigned long)h->magic);
    fail(PF_ERR_HEADER);
    return false;
  }
  if (h->version != PFRM_VERSION || h->header_len != PFRM_HEADER_LEN) {
    PF_LOGE("unsupported PFRM v%u len %u", h->version, h->header_len);
    fail(PF_ERR_HEADER);
    return false;
  }
  const uint32_t want = pf_crc32(0, buf_, 36);
  if (h->header_crc32 != want) {
    PF_LOGE("header crc %08lx != %08lx", (unsigned long)h->header_crc32,
            (unsigned long)want);
    fail(PF_ERR_HEADER);
    return false;
  }
  if (h->width != PF_PANEL_W || h->height != PF_PANEL_H) {
    PF_LOGE("image is %ux%u, panel is %ux%u", h->width, h->height, PF_PANEL_W, PF_PANEL_H);
    fail(PF_ERR_HEADER);
    return false;
  }
  if (h->format != PFRM_FMT_SEEED_GFX_4BPP || h->palette_id != PFRM_PALETTE_SPECTRA6) {
    PF_LOGE("unsupported format %u palette %lu", h->format, (unsigned long)h->palette_id);
    fail(PF_ERR_HEADER);
    return false;
  }
  if (h->data_len != PF_PIXEL_BYTES ||
      (size_t)h->data_len + PFRM_HEADER_LEN != expected_) {
    PF_LOGE("data_len %lu inconsistent with transfer size %u",
            (unsigned long)h->data_len, (unsigned)expected_);
    fail(PF_ERR_SIZE);
    return false;
  }
  header_ok_ = true;
  return true;
}

bool ImageBuffer::validate_payload() {
  if (!complete()) {
    PF_LOGE("truncated: %u of %u bytes", (unsigned)received_, (unsigned)expected_);
    fail(PF_ERR_SIZE);
    return false;
  }
  const PfrmHeader* h = header();
  // Trust the running CRC only if it actually consumed every payload byte exactly
  // once. Anything else (a retried chunk, a gap) falls back to a single pass, which
  // costs about 100 ms and is cheap against a 30-second wrong refresh.
  const bool incremental_ok = !crc_dirty_ && crc_next_ == h->data_len;
  const uint32_t got = incremental_ok ? crc_ : pf_crc32(0, pixels(), h->data_len);
  if (got != h->data_crc32) {
    PF_LOGE("payload crc %08lx != %08lx", (unsigned long)got, (unsigned long)h->data_crc32);
    fail(PF_ERR_CRC);
    return false;
  }
  return true;
}

bool ImageBuffer::validate_all() {
  return validate_header() && validate_payload();
}

}  // namespace pf
