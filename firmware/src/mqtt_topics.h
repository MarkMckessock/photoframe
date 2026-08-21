// MQTT topic map.
//
// MQTT is the control plane ONLY. The image itself is fetched over HTTP with a
// conditional GET -- see net/image_client.h for why. Nothing here ever carries more
// than a few hundred bytes.
//
//   TOPIC                                RET  QOS  DIR   PAYLOAD
//   home/<dev>/availability               Y    1   dev   "online" | "asleep" | "offline" (LWT)
//   home/<dev>/state                      Y    1   dev   status JSON, see mqtt.cpp
//   home/<dev>/cmd/config                 Y    1   srv   {"poll_seconds":900,"night_start":22,"night_end":7}
//   home/<dev>/cmd/ota                    Y    1   srv   {"version":"1.2.0","url":"...","sha256":"..."}
//   home/<dev>/cmd/clear                  N    1   srv   ""  -- forget the stored ETag, refetch next wake
//   homeassistant/sensor/<dev>_*/config   Y    1   dev   HA discovery
//
// Commands are retained on purpose: the device is asleep when you publish them, so
// the retained value is the authoritative desired state it reads on its next wake.
// (Same reasoning as light/registry in dmx-engine.)
//
// availability is three-valued so that "offline" -- the LWT -- means "died
// unexpectedly", which is actionable, rather than "asleep", which is not.
#pragma once

#include "secrets.h"

#define PF_TOPIC_ROOT         "home/" DEVICE_INSTANCE_NAME

#define PF_TOPIC_AVAILABILITY PF_TOPIC_ROOT "/availability"
#define PF_TOPIC_STATE        PF_TOPIC_ROOT "/state"
#define PF_TOPIC_CMD_WILDCARD PF_TOPIC_ROOT "/cmd/#"
#define PF_TOPIC_CMD_CONFIG   PF_TOPIC_ROOT "/cmd/config"
#define PF_TOPIC_CMD_OTA      PF_TOPIC_ROOT "/cmd/ota"
#define PF_TOPIC_CMD_CLEAR    PF_TOPIC_ROOT "/cmd/clear"

#define PF_AVAIL_ONLINE  "online"
#define PF_AVAIL_ASLEEP  "asleep"
#define PF_AVAIL_OFFLINE "offline"

#define PF_HA_DISCOVERY_PREFIX "homeassistant"
// Bump when the discovery payloads change shape, so devices republish them.
#define PF_HA_DISCOVERY_VERSION 1
