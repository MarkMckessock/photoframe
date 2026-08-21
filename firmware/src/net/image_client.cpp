#include "image_client.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClient.h>

#include "core/logger.h"
#include "image/image_buffer.h"
#include "persist/rtc_state.h"
#include "pf_config.h"
#include "pf_image_format.h"

namespace pf {
namespace net {
namespace image {

const char* result_name(Result r) {
  switch (r) {
    case Result::NotModified: return "not_modified";
    case Result::Updated:     return "updated";
    case Result::Error:       return "error";
  }
  return "?";
}

Result fetch(ImageBuffer& buf, const char* url, const char* if_none_match,
             char* out_etag, size_t etag_len) {
  if (out_etag && etag_len) out_etag[0] = '\0';

  WiFiClient client;
  HTTPClient http;
  http.setConnectTimeout(PF_HTTP_CONNECT_MS);
  http.setTimeout(PF_HTTP_CONNECT_MS);
  http.setReuse(false);

  if (!http.begin(client, url)) {
    PF_LOGE("http: cannot parse %s", url);
    return Result::Error;
  }

  static const char* kCollect[] = {"ETag"};
  http.collectHeaders(kCollect, 1);
  if (if_none_match && if_none_match[0]) {
    http.addHeader("If-None-Match", if_none_match);
  }

  const int code = http.GET();
  if (code == HTTP_CODE_NOT_MODIFIED) {
    http.end();
    return Result::NotModified;
  }
  if (code != HTTP_CODE_OK) {
    PF_LOGE("http: %s -> %d (%s)", url, code,
            code < 0 ? http.errorToString(code).c_str() : "");
    http.end();
    return Result::Error;
  }

  const int declared = http.getSize();
  if (declared <= 0) {
    // We need Content-Length: without it we cannot size the buffer up front, and a
    // chunked response would let a truncation masquerade as a complete image.
    PF_LOGE("http: no usable Content-Length (%d)", declared);
    http.end();
    return Result::Error;
  }
  if (!buf.begin((size_t)declared)) {
    http.end();
    return Result::Error;
  }

  String etag = http.header("ETag");

  WiFiClient* stream = http.getStreamPtr();
  size_t offset = 0;
  bool header_checked = false;
  uint32_t last_progress = millis();
  const uint32_t hard_deadline = millis() + PF_HTTP_TOTAL_MS;

  while (offset < (size_t)declared) {
    if (millis() > hard_deadline) {
      PF_LOGE("http: exceeded %ums total budget at %u/%d bytes",
              (unsigned)PF_HTTP_TOTAL_MS, (unsigned)offset, declared);
      http.end();
      return Result::Error;
    }
    const int avail = stream->available();
    if (avail <= 0) {
      if (!client.connected() && offset < (size_t)declared) {
        PF_LOGE("http: connection dropped at %u/%d bytes", (unsigned)offset, declared);
        http.end();
        return Result::Error;
      }
      // A stall is the failure mode a plain total-timeout misses: a server that
      // accepts the connection and then goes quiet would otherwise hold the radio on
      // for the whole budget.
      if (millis() - last_progress > PF_HTTP_STALL_MS) {
        PF_LOGE("http: stalled for %ums at %u/%d bytes", (unsigned)PF_HTTP_STALL_MS,
                (unsigned)offset, declared);
        http.end();
        return Result::Error;
      }
      delay(5);
      continue;
    }

    size_t want = (size_t)avail;
    if (offset + want > (size_t)declared) want = (size_t)declared - offset;
    uint8_t* dst = buf.write_ptr(offset);
    if (!dst) {
      http.end();
      return Result::Error;
    }
    const int got = stream->readBytes(dst, want);
    if (got <= 0) {
      delay(5);
      continue;
    }
    if (!buf.commit(offset, (size_t)got)) {
      http.end();
      return Result::Error;
    }
    offset += (size_t)got;
    last_progress = millis();

    // Check the header the moment we have it. A wrong-format or wrong-dimension blob
    // should cost 64 bytes of radio time, not a megabyte of it.
    if (!header_checked && buf.header_ready()) {
      header_checked = true;
      if (!buf.validate_header()) {
        http.end();
        return Result::Error;
      }
    }
  }
  http.end();

  if (!buf.validate_payload()) return Result::Error;

  if (out_etag && etag_len && etag.length()) {
    strncpy(out_etag, etag.c_str(), etag_len - 1);
    out_etag[etag_len - 1] = '\0';
  }
  PF_LOGI("http: fetched %u bytes, etag %s", (unsigned)offset,
          etag.length() ? etag.c_str() : "(none)");
  return Result::Updated;
}

}  // namespace image
}  // namespace net
}  // namespace pf
