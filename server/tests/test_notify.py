"""Notification behaviour: fire on the events that matter, stay quiet otherwise."""

import json
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from server.notify import Notifier  # noqa: E402


@pytest.fixture()
def notifier(tmp_path, monkeypatch):
    monkeypatch.setenv("NOTIFY_ENABLED", "true")
    monkeypatch.setenv("PUSHOVER_TOKEN", "tok")
    monkeypatch.setenv("PUSHOVER_USER", "usr")
    n = Notifier(tmp_path, "home/photoframe", "", 1883)
    n.sent = []
    n.send = lambda title, message, priority=0: n.sent.append((title, message, priority)) or True
    return n


def state(**kw):
    d = {"result": "no_change", "etag": '"a"', "battery_mv": 4100,
         "battery_pct": 90, "wake_count": 1}
    d.update(kw)
    return d


def test_fires_when_a_new_photo_is_actually_rendered(notifier):
    notifier.handle_state(state(result="rendered", etag='"new"'))
    assert len(notifier.sent) == 1
    assert "showing a new photo" in notifier.sent[0][1]


def test_does_not_fire_on_a_no_change_wake(notifier):
    notifier.handle_state(state(result="no_change", etag='"new"'))
    assert notifier.sent == []


def test_does_not_re_announce_the_same_photo(notifier):
    """The frame republishes state on every wake; only the change is interesting."""
    notifier.handle_state(state(result="rendered", etag='"x"'))
    notifier.handle_state(state(result="rendered", etag='"x"'))
    assert len(notifier.sent) == 1


def test_battery_alert_fires_once_and_uses_hysteresis(notifier):
    notifier.handle_state(state(battery_mv=3450))
    assert len(notifier.sent) == 1
    assert notifier.sent[0][2] == 1                      # high priority

    # Jittering around the threshold must not re-fire -- that is how an alert becomes
    # noise you learn to ignore.
    for mv in (3480, 3400, 3520, 3499):
        notifier.handle_state(state(battery_mv=mv))
    assert len(notifier.sent) == 1

    notifier.handle_state(state(battery_mv=3850))        # past the clear threshold
    assert len(notifier.sent) == 2
    assert "recovered" in notifier.sent[1][0]

    notifier.handle_state(state(battery_mv=3450))        # re-armed
    assert len(notifier.sent) == 3


def test_state_survives_a_restart(tmp_path, monkeypatch):
    """A pod restart must not re-announce the current photo or re-fire a live alert."""
    monkeypatch.setenv("NOTIFY_ENABLED", "true")
    monkeypatch.setenv("PUSHOVER_TOKEN", "t"); monkeypatch.setenv("PUSHOVER_USER", "u")

    a = Notifier(tmp_path, "home/photoframe", "", 1883)
    a.sent = []; a.send = lambda *x, **k: a.sent.append(x) or True
    a.handle_state(state(result="rendered", etag='"keep"', battery_mv=3400))
    assert len(a.sent) == 2                              # photo + battery

    b = Notifier(tmp_path, "home/photoframe", "", 1883)  # fresh process, same volume
    b.sent = []; b.send = lambda *x, **k: b.sent.append(x) or True
    b.handle_state(state(result="rendered", etag='"keep"', battery_mv=3400))
    assert b.sent == []


def test_single_toggle_silences_everything(tmp_path, monkeypatch):
    monkeypatch.delenv("NOTIFY_ENABLED", raising=False)
    n = Notifier(tmp_path, "home/photoframe", "", 1883)
    assert n.enabled is False
    assert n.send("t", "m") is False                     # refuses to send
    n.handle_state(state(result="rendered", etag='"z"', battery_mv=3000))
    assert not (tmp_path / "notify.json").exists()       # and records nothing


def test_missing_credentials_do_not_raise(tmp_path, monkeypatch):
    monkeypatch.setenv("NOTIFY_ENABLED", "true")
    monkeypatch.delenv("PUSHOVER_TOKEN", raising=False)
    monkeypatch.delenv("PUSHOVER_USER", raising=False)
    n = Notifier(tmp_path, "home/photoframe", "", 1883)
    assert n.send("t", "m") is False                     # logs and moves on
