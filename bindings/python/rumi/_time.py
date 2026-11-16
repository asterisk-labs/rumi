import datetime as dt
import re
from collections.abc import Set
from itertools import islice

import numpy as np

from ._ffi import RumiFormatError

INTERVAL, INSTANT = 1, 2

DAY = 86400
EPOCH = dt.date(1970, 1, 1)
EPOCH_UTC = dt.datetime(1970, 1, 1, tzinfo=dt.UTC)

# 0001-01-01 and 9999-12-31T23:59:59 UTC, the span Python's datetime holds.
FIRST_SECOND, LAST_SECOND = -62135596800, 253402300799

# The ISO grammar the R and Julia bindings accept.
_ISO_TIME = re.compile(
    r"([0-9]{4})-([0-9]{2})-([0-9]{2})(?:[T ]([0-9]{2}):([0-9]{2})"
    r"(?::([0-9]{2})(?:[.,]([0-9]+))?)?(Z|z|[+-][0-9]{2}(?::?[0-9]{2})?)?)?")


def _iso_seconds(text):
    """Convert ISO text to POSIX seconds, reading a naive time as UTC."""
    match = _ISO_TIME.fullmatch(text)
    if match is None:
        raise ValueError(f"{text!r} is not an ISO date or datetime")
    year, month, day, *clock, fraction, zone = match.groups()
    hour, minute, second = (int(v or 0) for v in clock)
    if hour > 23 or minute > 59 or second > 59:
        raise ValueError(f"{text!r} is not an ISO date or datetime")
    if fraction is not None and fraction.strip("0"):
        raise ValueError(
            f"{text!r} carries a fraction of a second; rumi records whole "
            f"seconds; round the value before writing")
    # The calendar repeats every 400 years, so year 0 is checked as 2000.
    try:
        days = (dt.date(2000 + int(year) % 400, int(month), int(day)) - EPOCH).days
    except ValueError:
        raise ValueError(f"{text!r} is not an ISO date or datetime") from None
    days += (int(year) // 400 - 5) * 146097
    seconds = days * DAY + hour * 3600 + minute * 60 + second
    if zone is None or zone in ("Z", "z"):
        return seconds
    digits = zone[1:].replace(":", "")
    hours, minutes = int(digits[:2]), int(digits[2:] or 0)
    if hours > 23 or minutes > 59:
        raise ValueError(f"{text!r} has an invalid UTC offset")
    # An offset names local time, and rumi records UTC.
    offset = hours * 3600 + minutes * 60
    return seconds + offset if zone[0] == "-" else seconds - offset


def _to_seconds(value):
    """Convert one supported time coordinate to POSIX seconds."""
    if isinstance(value, str):
        return _iso_seconds(value)
    if isinstance(value, np.datetime64):
        # NumPy datetime64 values must fall on a whole second.
        whole = value.astype("datetime64[s]")
        if whole.astype(value.dtype) != value:
            raise ValueError(
                f"{value} carries a fraction of a second; rumi records whole "
                f"seconds; round the value before writing")
        # Seconds, not a datetime, so the core can refuse a year past 9999.
        return int(whole.astype(np.int64))

    if isinstance(value, dt.datetime):
        # The format uses UTC; naive datetimes are interpreted as UTC.
        if value.tzinfo is not None:
            value = value.astimezone(dt.UTC)
        if value.microsecond:
            raise ValueError(
                f"{value.isoformat()} carries a fraction of a second; rumi "
                f"records whole seconds; round the value before writing")
        return int(value.replace(tzinfo=dt.UTC).timestamp())
    if isinstance(value, dt.date):
        return (value - EPOCH).days * DAY
    raise TypeError(
        f"a time coordinate is a date, a datetime or an ISO string, got "
        f"{type(value).__name__}")


def _axis_entries(time, steps):
    """Validate the outer sequence and classify its entries."""
    if isinstance(time, Set):
        raise TypeError("time must be ordered; a set cannot preserve time order")
    if isinstance(time, tuple):
        raise TypeError(
            "a tuple is one step's start and end, so it cannot be the list of "
            "steps; wrap it as time=[(start, end)]")
    if isinstance(time, (str, bytes)) or not hasattr(time, "__iter__"):
        raise TypeError(
            f"time is a list with one entry per time step; wrap a single "
            f"coordinate as time=[{time!r}]")

    # Limit generators to one more step than expected.
    entries = list(islice(time, steps + 1))
    intervals = bool(entries) and isinstance(entries[0], tuple)
    if any(isinstance(entry, tuple) != intervals for entry in entries):
        raise ValueError(
            "a file records one kind of time step, so the list is either all "
            "plain coordinates (instants) or all pairs (intervals), not a mix")

    kind = INTERVAL if intervals else INSTANT
    if len(entries) > steps:
        what = "an interval" if kind == INTERVAL else "an instant"
        raise ValueError(
            f"{what} axis needs one entry per time step, so {steps} of "
            f"them; got more than {steps}")
    return entries, kind


def _flatten(entries, kind):
    """Flatten interval pairs while preserving their step order."""
    if kind == INSTANT:
        return entries

    coordinates = []
    for i, entry in enumerate(entries):
        if len(entry) != 2:
            raise ValueError(
                f"time step {i} is a pair of a start and an end, got "
                f"{len(entry)} values")
        coordinates.extend(entry)
    return coordinates


def compile_axis(time, steps):
    """Return ``(time_type, coordinates)`` in POSIX seconds."""
    if time is None:
        raise TypeError(
            "every file labels its time steps; pass one date per step, or a "
            "(start, end) pair for a step without a single instant")

    entries, kind = _axis_entries(time, steps)
    return kind, tuple(_to_seconds(value)
                       for value in _flatten(entries, kind))


def _ordinal(n):
    suffix = "th" if 10 <= n % 100 <= 20 else {1: "st", 2: "nd", 3: "rd"}.get(n % 10, "th")
    return f"{n}{suffix}"


def from_seconds_axis(seconds, kind):
    """Decode one axis as dates or as timezone-aware UTC datetimes."""
    # Rumi writes only this span, but a file from another writer may not keep to it.
    for i, value in enumerate(seconds):
        if not FIRST_SECOND <= value <= LAST_SECOND:
            step = i // 2 if kind == INTERVAL else i
            raise RumiFormatError(
                f"the {_ordinal(step + 1)} time step falls outside the years 1 to 9999")
    if all(value % DAY == 0 for value in seconds):
        return [EPOCH + dt.timedelta(days=value // DAY) for value in seconds]
    return [EPOCH_UTC + dt.timedelta(seconds=value) for value in seconds]
