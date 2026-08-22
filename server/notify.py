"""Pushover notifications for the photo frame.

Two events are worth a phone buzz and no more than that:

  - the frame has actually *displayed* a new photo (not merely received one -- the
    sender already gets an SMS reply, and a receipt notification would fire minutes
    before anything appears on the wall);
  - the battery has fallen below a level where it will soon stop rendering.

Everything is off unless NOTIFY_ENABLED is set, so deploying this cannot start
messaging anyone before you have decided it should.

State lives in the same volume as the image so it survives restarts: without it, a
pod restart would re-announce the current photo and re-fire a battery alert that is
still active.
"""

import json
import logging
import os
import threading
import time
from pathlib import Path

import requests

logger = logging.getLogger("photoframe.notify")

PUSHOVER_URL = "https://api.pushover.net/1/messages.json"


def _env_bool(name, default=False):
    return os.environ.get(name, str(default)).strip().lower() in ("1", "true", "yes", "on")


class Notifier:
    def __init__(self, store_dir, topic_root, mqtt_host, mqtt_port,
                 mqtt_username=None, mqtt_password=None):
        # One switch. Off by default, so deploying this cannot start messaging anyone
        # before you have decided it should.
        self.enabled = _env_bool("NOTIFY_ENABLED", False)

        # Hysteresis: alert below LOW, and do not re-arm until it climbs back past
        # CLEAR. A single threshold would re-fire every wake as the reading jitters
        # around it, which trains you to ignore the notification that matters.
        self.low_mv = int(os.environ.get("NOTIFY_BATTERY_LOW_MV", 3500))
        self.clear_mv = int(os.environ.get("NOTIFY_BATTERY_CLEAR_MV", 3800))

        self.token = os.environ.get("PUSHOVER_TOKEN", "")
        self.user = os.environ.get("PUSHOVER_USER", "")

        self.topic = f"{topic_root}/state"
        self.mqtt_host, self.mqtt_port = mqtt_host, mqtt_port
        self.mqtt_username, self.mqtt_password = mqtt_username, mqtt_password
        self.state_path = Path(store_dir) / "notify.json"
        self._lock = threading.Lock()

    # --- persistence -------------------------------------------------------
    def _load(self):
        try:
            return json.loads(self.state_path.read_text())
        except (OSError, ValueError):
            return {}

    def _save(self, state):
        try:
            self.state_path.parent.mkdir(parents=True, exist_ok=True)
            tmp = self.state_path.with_suffix(".tmp")
            tmp.write_text(json.dumps(state, indent=2))
            os.replace(tmp, self.state_path)
        except OSError:
            logger.exception("could not persist notification state")

    # --- sending -----------------------------------------------------------
    def send(self, title, message, priority=0):
        if not self.enabled:
            logger.info("notify suppressed (NOTIFY_ENABLED off): %s -- %s", title, message)
            return False
        if not (self.token and self.user):
            logger.warning("notify enabled but PUSHOVER_TOKEN/PUSHOVER_USER are unset")
            return False
        try:
            r = requests.post(PUSHOVER_URL, timeout=10, data={
                "token": self.token, "user": self.user,
                "title": title, "message": message, "priority": priority,
            })
            r.raise_for_status()
            logger.info("pushover sent: %s", title)
            return True
        except Exception:
            # Never let a notification failure affect the request that triggered it.
            logger.exception("pushover send failed")
            return False

    # --- event handling ----------------------------------------------------
    def handle_state(self, doc, describe_image=None):
        """Called for each state doc the frame publishes."""
        if not self.enabled:
            # One switch means one switch: when off, do not even record what would
            # have been announced. Otherwise enabling it later would silently swallow
            # the first notification, because the state file already names that photo.
            return
        with self._lock:
            state = self._load()
            changed = False

            etag = doc.get("etag") or ""
            if (doc.get("result") == "rendered" and etag
                    and etag != state.get("last_etag")):
                who = describe_image() if describe_image else None
                msg = f"The frame is now showing a new photo{f' from {who}' if who else ''}."
                self.send("Photo frame updated", msg)
                state["last_etag"] = etag
                changed = True

            mv = doc.get("battery_mv") or 0
            if mv:
                alerted = state.get("battery_alerted", False)
                if not alerted and mv < self.low_mv:
                    self.send("Photo frame battery low",
                              f"{mv} mV ({doc.get('battery_pct', '?')}%). It will stop "
                              f"refreshing below {self.low_mv} mV.", priority=1)
                    state["battery_alerted"] = True
                    changed = True
                elif alerted and mv >= self.clear_mv:
                    self.send("Photo frame battery recovered", f"Back to {mv} mV.")
                    state["battery_alerted"] = False
                    changed = True

            if changed:
                self._save(state)

    # --- background subscriber ---------------------------------------------
    def start(self, describe_image=None):
        """Watch the frame's retained state topic in a background thread.

        The frame publishes only on wake -- a handful of times an hour -- so this is
        almost entirely idle.
        """
        if not self.enabled:
            logger.info("notifications disabled; not starting the MQTT watcher")
            return
        if not self.mqtt_host:
            logger.warning("NOTIFY_ENABLED but MQTT_HOST is unset; no watcher")
            return
        threading.Thread(target=self._run, args=(describe_image,),
                         name="notify-mqtt", daemon=True).start()

    def _run(self, describe_image):
        import paho.mqtt.client as mqtt

        def on_connect(client, userdata, flags, reason_code, properties=None):
            client.subscribe(self.topic, qos=1)
            logger.info("notify watcher subscribed to %s", self.topic)

        def on_message(client, userdata, msg):
            try:
                self.handle_state(json.loads(msg.payload.decode("utf-8")), describe_image)
            except Exception:
                logger.exception("bad state doc")

        while True:
            try:
                c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,
                                client_id="photoframe-notify")
                if self.mqtt_username:
                    c.username_pw_set(self.mqtt_username, self.mqtt_password)
                c.on_connect = on_connect
                c.on_message = on_message
                c.connect(self.mqtt_host, self.mqtt_port, keepalive=60)
                c.loop_forever()
            except Exception:
                logger.exception("notify watcher died; retrying in 30s")
                time.sleep(30)
