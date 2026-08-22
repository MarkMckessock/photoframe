#include "mqtt.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <espMqttClient.h>
#include <string.h>

#include "core/logger.h"
#include "mqtt_topics.h"
#include "persist/nvs_store.h"
#include "persist/rtc_state.h"
#include "net/wifi_manager.h"
#include "pf_config.h"
#include "pf_version.h"
#include "secrets.h"

namespace pf {
namespace net {
namespace mqtt {
namespace {

espMqttClient g_client;

bool g_connected = false;
bool g_failed = false;
bool g_clear = false;
OtaRequest g_ota = {};

uint16_t g_pending_packet = 0;
bool g_pending_acked = false;

// Commands are small; anything larger is malformed and we would rather drop it than
// grow a buffer for it.
constexpr size_t kCmdMax = 512;
char g_cmd[kCmdMax];

void on_connect(bool session_present) {
  (void)session_present;
  g_connected = true;
  PF_LOGI("mqtt connected");
  g_client.publish(PF_TOPIC_AVAILABILITY, 1, true, PF_AVAIL_ONLINE);
  // Subscribe to the command tree only. There is deliberately nothing large anywhere
  // under this device's topic root, but the habit matters: a wildcard that happens to
  // match a bulk topic turns every wake into a needless download.
  g_client.subscribe(PF_TOPIC_CMD_WILDCARD, 1);
}

void on_disconnect(espMqttClientTypes::DisconnectReason reason) {
  g_connected = false;
  if (!g_failed) {
    PF_LOGW("mqtt disconnected (reason %u)", (unsigned)reason);
  }
}

void on_publish(uint16_t packet_id) {
  if (packet_id == g_pending_packet) g_pending_acked = true;
}

void handle_config(const char* payload) {
  JsonDocument doc;
  if (deserializeJson(doc, payload)) {
    PF_LOGW("cmd/config is not valid JSON; ignoring");
    return;
  }
  // Tolerant parsing, dmx-engine style: accept a couple of spellings, ignore anything
  // we do not recognise, and never let a malformed field wedge the device.
  uint32_t poll = doc["poll_seconds"] | 0U;
  if (!poll) poll = doc["poll_s"] | 0U;
  if (poll && poll != nvs::poll_seconds()) {
    nvs::set_poll_seconds(poll);
    PF_LOGI("config: poll_seconds -> %lu", (unsigned long)nvs::poll_seconds());
  }
  if (doc["night_start"].is<uint8_t>() && doc["night_end"].is<uint8_t>()) {
    const uint8_t s = doc["night_start"], e = doc["night_end"];
    if (s != nvs::night_start() || e != nvs::night_end()) {
      nvs::set_night(s, e);
      PF_LOGI("config: night -> %u..%u", s, e);
    }
  }
}

void handle_ota(const char* payload) {
  JsonDocument doc;
  if (deserializeJson(doc, payload)) return;
  const char* v = doc["version"] | "";
  const char* u = doc["url"] | "";
  const char* h = doc["sha256"] | "";
  if (!v[0] || !u[0]) return;

  strncpy(g_ota.version, v, sizeof(g_ota.version) - 1);
  strncpy(g_ota.url, u, sizeof(g_ota.url) - 1);
  strncpy(g_ota.sha256, h, sizeof(g_ota.sha256) - 1);
  g_ota.valid = true;
}

void handle_clear(const char* payload) {
  // cmd/clear is retained and carries a token (a timestamp is fine). We act only when
  // the token differs from the one we last acted on -- otherwise a retained message
  // would re-trigger on every single wake, forever.
  char seen[40] = "";
  nvs::get_clear_token(seen, sizeof(seen));
  if (strncmp(seen, payload, sizeof(seen) - 1) == 0) return;
  nvs::set_clear_token(payload);
  g_clear = true;
  PF_LOGI("cmd/clear token %s", payload);
}

void on_message(const espMqttClientTypes::MessageProperties& props, const char* topic,
                const uint8_t* payload, size_t len, size_t index, size_t total) {
  (void)props;
  if (total >= kCmdMax) {
    PF_LOGW("%s: %u byte command is too large; dropped", topic, (unsigned)total);
    return;
  }
  memcpy(g_cmd + index, payload, len);
  if (index + len < total) return;  // more chunks to come
  g_cmd[total] = '\0';

  if (!strcmp(topic, PF_TOPIC_CMD_CONFIG))      handle_config(g_cmd);
  else if (!strcmp(topic, PF_TOPIC_CMD_OTA))    handle_ota(g_cmd);
  else if (!strcmp(topic, PF_TOPIC_CMD_CLEAR))  handle_clear(g_cmd);
  else PF_LOGW("unhandled command topic %s", topic);
}

bool publish_and_wait(const char* topic, uint8_t qos, bool retain, const char* payload,
                      uint32_t timeout_ms) {
  g_pending_packet = g_client.publish(topic, qos, retain, payload);
  if (g_pending_packet == 0) return qos == 0;  // QoS 0 returns 0 on success
  g_pending_acked = false;
  const uint32_t deadline = millis() + timeout_ms;
  while (millis() < deadline && !g_pending_acked) {
    g_client.loop();
    delay(2);
  }
  return g_pending_acked;
}

}  // namespace

bool connect(uint32_t timeout_ms) {
  g_failed = false;
  g_client.setServer(MQTT_SERVER, MQTT_PORT);
  g_client.setClientId(DEVICE_INSTANCE_NAME);
  g_client.setCleanSession(true);
  g_client.setKeepAlive(15);
  if (sizeof(MQTT_USER) > 1) {
    g_client.setCredentials(MQTT_USER, MQTT_PASSWORD);
  }
  // Retained LWT. "offline" therefore means "stopped talking without saying goodbye",
  // which is worth an alert; a normal sleep publishes "asleep" instead.
  g_client.setWill(PF_TOPIC_AVAILABILITY, 1, true, PF_AVAIL_OFFLINE);
  g_client.onConnect(on_connect);
  g_client.onDisconnect(on_disconnect);
  g_client.onMessage(on_message);
  g_client.onPublish(on_publish);

  g_client.connect();
  const uint32_t deadline = millis() + timeout_ms;
  while (millis() < deadline && !g_connected) {
    g_client.loop();
    delay(5);
  }
  if (!g_connected) {
    g_failed = true;
    PF_LOGE("mqtt: no connection to %s:%d in %lums", MQTT_SERVER, MQTT_PORT,
            (unsigned long)timeout_ms);
    g_client.disconnect(true);
  }
  return g_connected;
}

bool connected() { return g_connected; }

void pump(uint32_t ms) {
  const uint32_t deadline = millis() + ms;
  while (millis() < deadline) {
    g_client.loop();
    delay(2);
  }
}

bool clear_requested() { return g_clear; }
const OtaRequest& ota_request() { return g_ota; }

bool publish_state(const StateDoc& s) {
  if (!g_connected) return false;

  JsonDocument doc;
  doc["fw"] = PF_VERSION;
  doc["git"] = PF_GIT_SHA;
  // When this wake happened. rendered_at only moves on an actual render, so it cannot
  // date a no-change wake -- which is most of them.
  doc["ts"] = now_epoch();
  doc["wake_cause"] = s.wake_cause;
  doc["wake_count"] = s.wake_count;
  doc["total_renders"] = s.total_renders;
  doc["battery_mv"] = s.battery_mv;
  doc["battery_pct"] = s.battery_pct;
  doc["rssi"] = s.rssi;
  doc["ip"] = wifi::ip_str();
  doc["wifi_ms"] = s.wifi_ms;
  doc["awake_ms"] = s.awake_ms;
  doc["etag"] = s.etag ? s.etag : "";
  if (s.deferred_etag && s.deferred_etag[0]) doc["deferred"] = s.deferred_etag;
  doc["rendered_at"] = s.rendered_at;
  doc["last_render_ms"] = s.last_render_ms;
  doc["next_wake_s"] = s.next_wake_s;
  doc["result"] = s.result;
  doc["panel"] = s.panel;
  doc["consecutive_failures"] = s.consecutive_failures;
  if (s.error) doc["error"] = s.error; else doc["error"] = nullptr;
  if (s.prev_error) {
    doc["prev_error"] = s.prev_error;
    doc["prev_awake_ms"] = s.prev_awake_ms;
  }
  doc["free_psram"] = (uint32_t)ESP.getFreePsram();

  char buf[640];
  const size_t n = serializeJson(doc, buf, sizeof(buf));
  if (n == 0 || n >= sizeof(buf)) {
    PF_LOGW("state doc did not fit in %u bytes", (unsigned)sizeof(buf));
    return false;
  }
  return publish_and_wait(PF_TOPIC_STATE, 1, true, buf, 2000);
}

void publish_discovery_if_stale() {
  if (nvs::ha_discovery_version() == PF_HA_DISCOVERY_VERSION) return;

  struct Entity {
    const char* id;
    const char* name;
    const char* value_template;
    const char* device_class;
    const char* unit;
    const char* state_class;
  };
  static const Entity kEntities[] = {
      {"battery", "Battery", "{{ value_json.battery_mv | float / 1000 }}", "voltage",
       "V", "measurement"},
      {"battery_pct", "Battery level", "{{ value_json.battery_pct }}", "battery", "%",
       "measurement"},
      {"rssi", "Signal", "{{ value_json.rssi }}", "signal_strength", "dBm",
       "measurement"},
      {"status", "Status", "{{ value_json.result }}", nullptr, nullptr, nullptr},
      {"image", "Image", "{{ value_json.etag }}", nullptr, nullptr, nullptr},
  };

  // expire_after rather than availability_topic, deliberately. This device is offline
  // by design almost all the time; wiring availability into discovery would show every
  // entity as unavailable essentially always. Expiring at ~2.5x the poll interval means
  // entities go stale only when the frame has genuinely stopped checking in.
  const uint32_t expire = nvs::poll_seconds() * 5 / 2;

  for (const Entity& e : kEntities) {
    JsonDocument doc;
    doc["name"] = e.name;
    char uid[64];
    snprintf(uid, sizeof(uid), "%s_%s", DEVICE_INSTANCE_NAME, e.id);
    doc["unique_id"] = uid;
    doc["state_topic"] = PF_TOPIC_STATE;
    doc["value_template"] = e.value_template;
    doc["expire_after"] = expire;
    if (e.device_class) doc["device_class"] = e.device_class;
    if (e.unit) doc["unit_of_measurement"] = e.unit;
    if (e.state_class) doc["state_class"] = e.state_class;

    JsonObject dev = doc["device"].to<JsonObject>();
    dev["identifiers"][0] = DEVICE_INSTANCE_NAME;
    dev["name"] = "Photo Frame";
    dev["manufacturer"] = "Seeed";
    dev["model"] = "XIAO ePaper EE02 / Spectra 6 13.3\"";
    dev["sw_version"] = PF_VERSION;

    char topic[128];
    snprintf(topic, sizeof(topic), PF_HA_DISCOVERY_PREFIX "/sensor/%s_%s/config",
             DEVICE_INSTANCE_NAME, e.id);
    char buf[640];
    const size_t n = serializeJson(doc, buf, sizeof(buf));
    if (n && n < sizeof(buf)) {
      publish_and_wait(topic, 1, true, buf, 1500);
    }
  }
  nvs::set_ha_discovery_version(PF_HA_DISCOVERY_VERSION);
  PF_LOGI("published HA discovery v%u", PF_HA_DISCOVERY_VERSION);
}

void disconnect() {
  if (!g_connected) return;
  publish_and_wait(PF_TOPIC_AVAILABILITY, 1, true, PF_AVAIL_ASLEEP, 1000);
  g_client.disconnect(false);
  const uint32_t deadline = millis() + 500;
  while (millis() < deadline && g_connected) {
    g_client.loop();
    delay(5);
  }
}

}  // namespace mqtt
}  // namespace net
}  // namespace pf
