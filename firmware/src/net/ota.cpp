#include "ota.h"

#include <Arduino.h>
#include <ArduinoOTA.h>
#include <HTTPClient.h>
#include <Update.h>
#include <WiFiClient.h>
#include <esp_ota_ops.h>
#include <mbedtls/sha256.h>

#include "core/logger.h"
#include "pf_config.h"
#include "pf_version.h"
#include "secrets.h"

namespace pf {
namespace net {
namespace ota {
namespace {

bool g_pending = false;

bool hex_eq(const uint8_t* digest, const char* hex) {
  char buf[65];
  for (int i = 0; i < 32; i++) snprintf(buf + i * 2, 3, "%02x", digest[i]);
  buf[64] = '\0';
  return strcasecmp(buf, hex) == 0;
}

}  // namespace

void note_boot() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t state;
  if (esp_ota_get_state_partition(running, &state) == ESP_OK &&
      state == ESP_OTA_IMG_PENDING_VERIFY) {
    g_pending = true;
    PF_LOGW("running an unconfirmed image; it will roll back unless this wake succeeds");
  }
}

bool pending_verify() { return g_pending; }

void mark_good() {
  if (!g_pending) return;
  if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
    PF_LOGI("image confirmed good; rollback cancelled");
    g_pending = false;
  }
}

bool maybe_update(const char* desired_version, const char* url, const char* sha256_hex) {
  if (!desired_version || !desired_version[0] || !url || !url[0]) return false;
  if (strcmp(desired_version, PF_VERSION) == 0) return false;
  if (!sha256_hex || strlen(sha256_hex) != 64) {
    // Refusing an unverified update is the conservative choice: a truncated or
    // man-in-the-middled image on a device you can only reach over the air is a
    // problem you cannot fix from the sofa.
    PF_LOGE("ota %s: refusing, sha256 missing or malformed", desired_version);
    return false;
  }

  PF_LOGI("ota: %s -> %s from %s", PF_VERSION, desired_version, url);

  WiFiClient client;
  HTTPClient http;
  http.setConnectTimeout(PF_HTTP_CONNECT_MS);
  http.setTimeout(PF_HTTP_CONNECT_MS);
  if (!http.begin(client, url)) return false;

  const int code = http.GET();
  const int len = http.getSize();
  if (code != HTTP_CODE_OK || len <= 0) {
    PF_LOGE("ota: HTTP %d len %d", code, len);
    http.end();
    return false;
  }
  if (!Update.begin((size_t)len, U_FLASH)) {
    PF_LOGE("ota: Update.begin failed (%s)", Update.errorString());
    http.end();
    return false;
  }

  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, 0);

  uint8_t chunk[1024];
  WiFiClient* stream = http.getStreamPtr();
  int remaining = len;
  uint32_t last_progress = millis();
  while (remaining > 0) {
    const int avail = stream->available();
    if (avail <= 0) {
      if (millis() - last_progress > PF_HTTP_STALL_MS || !client.connected()) {
        PF_LOGE("ota: stalled with %d bytes to go", remaining);
        Update.abort();
        mbedtls_sha256_free(&sha);
        http.end();
        return false;
      }
      delay(5);
      continue;
    }
    const int want = (avail > (int)sizeof(chunk)) ? (int)sizeof(chunk) : avail;
    const int got = stream->readBytes(chunk, want > remaining ? remaining : want);
    if (got <= 0) { delay(5); continue; }
    mbedtls_sha256_update(&sha, chunk, got);
    if (Update.write(chunk, got) != (size_t)got) {
      PF_LOGE("ota: flash write failed (%s)", Update.errorString());
      Update.abort();
      mbedtls_sha256_free(&sha);
      http.end();
      return false;
    }
    remaining -= got;
    last_progress = millis();
  }
  http.end();

  uint8_t digest[32];
  mbedtls_sha256_finish(&sha, digest);
  mbedtls_sha256_free(&sha);

  // Check the hash *before* Update.end(), because end() is what makes the new image
  // bootable. Abort here and the current firmware simply carries on.
  if (!hex_eq(digest, sha256_hex)) {
    PF_LOGE("ota: sha256 mismatch; aborting");
    Update.abort();
    return false;
  }
  if (!Update.end(true)) {
    PF_LOGE("ota: Update.end failed (%s)", Update.errorString());
    return false;
  }

  PF_LOGI("ota: installed %s, rebooting", desired_version);
  delay(100);
  ESP.restart();
  return true;
}

void maintenance_window() {
  ArduinoOTA.setHostname(DEVICE_INSTANCE_NAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]() { PF_LOGI("ArduinoOTA: starting"); });
  ArduinoOTA.onEnd([]() { PF_LOGI("ArduinoOTA: done"); });
  ArduinoOTA.onError([](ota_error_t e) { PF_LOGE("ArduinoOTA: error %u", (unsigned)e); });
  ArduinoOTA.begin();

  PF_LOGI("maintenance window open for %lus on %s.local:3232",
          (unsigned long)(PF_MAINTENANCE_MS / 1000), DEVICE_INSTANCE_NAME);
  const uint32_t deadline = millis() + PF_MAINTENANCE_MS;
  while (millis() < deadline) {
    ArduinoOTA.handle();
    delay(10);
  }
  PF_LOGI("maintenance window closed");
}

}  // namespace ota
}  // namespace net
}  // namespace pf
