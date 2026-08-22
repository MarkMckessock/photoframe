"""Phone-number-to-name mapping, and the privacy properties that motivate it."""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from server.contacts import display_name, load  # noqa: E402

MAP = '{"+1 (555) 123-4567": "Alice", "15559876543": "Bob"}'


def test_resolves_known_numbers_regardless_of_formatting():
    c = load(MAP)
    for number in ("+15551234567", "15551234567", "555-123-4567", "(555) 123 4567"):
        assert display_name(number, c) == "Alice", number


def test_unknown_numbers_are_masked_never_shown_in_full():
    """A notification on a lock screen must not disclose somebody's number."""
    c = load(MAP)
    out = display_name("+15105551234", c)
    assert out == "…1234"
    assert "5105551234" not in out
    assert "+1510" not in out


def test_missing_or_empty_number_degrades_gracefully():
    c = load(MAP)
    assert display_name("", c) == "someone"
    assert display_name(None, c) == "someone"


def test_malformed_config_does_not_break_the_service():
    """A bad CONTACTS value must never take the webhook down."""
    assert load("{not json") == {}
    assert load("[1,2,3]") == {}          # right JSON, wrong shape
    assert load("") == {}
    assert display_name("+15551234567", {}) == "…4567"


def test_non_string_names_are_ignored():
    assert load('{"+15551234567": 42}') == {}


def test_env_var_is_the_source_when_no_map_is_passed(monkeypatch):
    monkeypatch.setenv("CONTACTS", MAP)
    assert display_name("+15551234567") == "Alice"
    monkeypatch.delenv("CONTACTS")
    assert display_name("+15551234567") == "…4567"
