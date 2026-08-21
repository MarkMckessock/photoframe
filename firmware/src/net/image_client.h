// One conditional GET, and everything that can go wrong with it.
//
// Why HTTP and not MQTT, given that the control plane is MQTT: a 960 KB image is a
// file, not a message. `If-None-Match` gives us the "has anything changed?" check for
// about two hundred bytes, which is the overwhelmingly common case and the one that
// decides battery life. Carrying the same payload as a retained MQTT message would
// mean parking a megabyte in the broker forever, re-sending it to any wildcard
// subscriber, hand-rolling a second metadata topic to avoid pulling it every wake,
// and having no way to resume a transfer that dies at 900 KB.
#pragma once

#include <stdint.h>
#include <stddef.h>

namespace pf {

class ImageBuffer;

namespace net {
namespace image {

enum class Result : uint8_t {
  NotModified,  // 304 -- nothing to do, go back to sleep. The cheap, common path.
  Updated,      // 200, downloaded and validated. Safe to render.
  Error,
};

// `if_none_match` may be empty to force a full fetch. On Updated, `out_etag` receives
// the server's ETag; store it only *after* a successful render, so that a crash
// between download and refresh re-fetches instead of silently skipping the photo.
Result fetch(ImageBuffer& buf, const char* url, const char* if_none_match,
             char* out_etag, size_t etag_len);

const char* result_name(Result r);

}  // namespace image
}  // namespace net
}  // namespace pf
