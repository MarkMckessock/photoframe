#include "image_cache.h"

#include <Arduino.h>
#include <LittleFS.h>

#include "core/logger.h"
#include "image/image_buffer.h"
#include "pf_config.h"

namespace pf {
namespace cache {
namespace {
bool g_mounted = false;
}

bool begin() {
  if (g_mounted) return true;
  // format_on_fail: a fresh board has no filesystem, and a corrupt one is worth
  // discarding -- the only thing in here is a file we can always re-download.
  if (!LittleFS.begin(/*formatOnFail=*/true)) {
    PF_LOGW("LittleFS mount failed; running without a cache");
    return false;
  }
  g_mounted = true;
  return true;
}

bool exists() { return g_mounted && LittleFS.exists(PF_CACHE_PATH); }

bool save(const uint8_t* blob, size_t len) {
  if (!g_mounted) return false;

  LittleFS.remove(PF_CACHE_TMP_PATH);
  File f = LittleFS.open(PF_CACHE_TMP_PATH, FILE_WRITE);
  if (!f) {
    PF_LOGW("cache: cannot open %s", PF_CACHE_TMP_PATH);
    return false;
  }
  const size_t wrote = f.write(blob, len);
  f.close();
  if (wrote != len) {
    PF_LOGW("cache: short write %u of %u", (unsigned)wrote, (unsigned)len);
    LittleFS.remove(PF_CACHE_TMP_PATH);
    return false;
  }
  LittleFS.remove(PF_CACHE_PATH);
  if (!LittleFS.rename(PF_CACHE_TMP_PATH, PF_CACHE_PATH)) {
    PF_LOGW("cache: rename failed");
    return false;
  }
  PF_LOGI("cache: stored %u bytes", (unsigned)len);
  return true;
}

bool load(ImageBuffer& buf) {
  if (!exists()) return false;

  File f = LittleFS.open(PF_CACHE_PATH, FILE_READ);
  if (!f) return false;
  const size_t len = f.size();
  if (!buf.begin(len)) {
    f.close();
    LittleFS.remove(PF_CACHE_PATH);
    return false;
  }
  const size_t got = f.read(buf.raw(), len);
  f.close();
  if (got != len) {
    PF_LOGW("cache: short read %u of %u", (unsigned)got, (unsigned)len);
    LittleFS.remove(PF_CACHE_PATH);
    return false;
  }
  if (!buf.adopt(len) || !buf.validate_all()) {
    PF_LOGW("cache: contents did not validate (%s); discarding",
            error_name(buf.error()));
    LittleFS.remove(PF_CACHE_PATH);
    return false;
  }
  PF_LOGI("cache: loaded %u bytes", (unsigned)len);
  return true;
}

}  // namespace cache
}  // namespace pf
