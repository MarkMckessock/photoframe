"""Map sender phone numbers to friendly names, without putting either in git.

Both repos are public, so the mapping travels the same route as every other secret:
a JSON field in 1Password -> ExternalSecret -> env var. Nothing on disk, nothing in
a commit, and rotating a name is a 1Password edit rather than a deploy.

    CONTACTS='{"+15551234567": "Alice", "+15559876543": "Bob"}'

Unknown numbers are never rendered in full. They degrade to a masked form (…4567),
which is enough to recognise a repeat sender or add them to the map, without a
notification on a lock screen -- or a log line, or a screen share -- disclosing
somebody's number.
"""

import json
import logging
import os
import re

logger = logging.getLogger("photoframe.contacts")


def _digits(number):
    return re.sub(r"\D", "", number or "")


def load(raw=None):
    """Parse the CONTACTS mapping. A malformed value must not break the service."""
    raw = os.environ.get("CONTACTS", "") if raw is None else raw
    if not raw.strip():
        return {}
    try:
        parsed = json.loads(raw)
    except ValueError:
        logger.warning("CONTACTS is not valid JSON; continuing without names")
        return {}
    if not isinstance(parsed, dict):
        logger.warning("CONTACTS must be a JSON object of number -> name")
        return {}

    # Key on the last 10 digits so +1-555-123-4567, 15551234567 and (555) 123-4567
    # all resolve. Twilio sends E.164, but the mapping is typed by a human.
    out = {}
    for number, name in parsed.items():
        d = _digits(number)
        if d and isinstance(name, str):
            out[d[-10:]] = name
    return out


def display_name(number, contacts=None):
    """A name if we know them, otherwise a masked number. Never the full number."""
    contacts = load() if contacts is None else contacts
    d = _digits(number)
    if not d:
        return "someone"
    name = contacts.get(d[-10:])
    return name if name else f"…{d[-4:]}"
