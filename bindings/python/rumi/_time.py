import datetime as dt
from typing import NamedTuple

from ._ffi import PathLike, _check, _enc, ffi, lib

UNDEFINED, INTERVAL, INSTANT = 0, 1, 2

DAY = 86400
EPOCH = dt.date(1970, 1, 1)


def _to_seconds(value):
    """Convert one supported time coordinate to POSIX seconds."""
    if isinstance(value, str):
        # datetime.fromisoformat also accepts dates, so try date first.
        try:
            value = dt.date.fromisoformat(value)
        except ValueError:
            try:
                value = dt.datetime.fromisoformat(value)
            except ValueError:
                raise ValueError(
                    f"{value!r} is not an ISO date or datetime") from None
    elif hasattr(value, "astype") and hasattr(value, "dtype"):
        # NumPy datetime64 values must fall on a whole second.
        whole = value.astype("datetime64[s]")
        if whole.astype(value.dtype) != value:
            raise ValueError(
                f"{value} carries a fraction of a second; rumi records whole "
                f"seconds, so round it yourself rather than have it dropped")
        value = whole.astype(dt.datetime)

    if isinstance(value, dt.datetime):
        # The format uses UTC; naive datetimes are interpreted as UTC.
        if value.tzinfo is not None:
            value = value.astimezone(dt.UTC)
        if value.microsecond:
            raise ValueError(
                f"{value.isoformat()} carries a fraction of a second; rumi "
                f"records whole seconds, so round it yourself rather than "
                f"have it dropped")
        return int(value.replace(tzinfo=dt.UTC).timestamp())
    if isinstance(value, dt.date):
        return (value - EPOCH).days * DAY
    raise TypeError(
        f"a time coordinate is a date, a datetime or an ISO string, got "
        f"{type(value).__name__}")


def _axis_entries(time, steps):
    """Validate the outer sequence and classify its entries."""
    if isinstance(time, tuple):
        raise TypeError(
            "a tuple is one step's start and end, so it cannot be the list of "
            "steps; wrap it as time=[(start, end)]")
    if isinstance(time, (str, bytes)) or not hasattr(time, "__iter__"):
        raise TypeError(
            f"time is a list with one entry per time step; wrap a single "
            f"coordinate as time=[{time!r}]")

    entries = list(time)
    if not entries:
        raise ValueError(
            f"a time axis covers every step, so {steps} coordinates are "
            f"needed; got an empty list")

    intervals = isinstance(entries[0], tuple)
    if any(isinstance(entry, tuple) != intervals for entry in entries):
        raise ValueError(
            "a file records one kind of time step, so the list is either all "
            "plain coordinates (instants) or all pairs (intervals), not a mix")

    kind = INTERVAL if intervals else INSTANT
    if len(entries) != steps:
        what = "interval" if kind == INTERVAL else "instant"
        raise ValueError(
            f"a {what} axis needs one entry per time step, so {steps} of "
            f"them; got {len(entries)}")
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
        return UNDEFINED, ()

    entries, kind = _axis_entries(time, steps)
    return kind, tuple(_to_seconds(value)
                       for value in _flatten(entries, kind))


def _from_seconds(seconds):
    """Return a date when possible, otherwise a UTC datetime."""
    if seconds % DAY == 0:
        return EPOCH + dt.timedelta(days=seconds // DAY)
    return dt.datetime.fromtimestamp(seconds, dt.UTC)


class Time(NamedTuple):
    """Decoded time axis.

    ``steps`` contains one coordinate per instant or one ``(start, end)`` pair
    per interval. ``kind`` is ``"instant"``, ``"interval"``, or ``None``.
    """

    steps: list
    kind: str | None


def read(path: PathLike) -> Time:
    """Read the time axis from a local rumi file."""
    kind = ffi.new("uint8_t*")
    _scale = ffi.new("uint32_t*")
    out = ffi.new("int64_t**")
    count = ffi.new("size_t*")
    _check(lib.rumi_read_time(_enc(path), kind, _scale, out, count))
    try:
        coords = [int(out[0][i]) for i in range(count[0])]
    finally:
        if out[0] != ffi.NULL:
            lib.rumi_free(out[0])

    if kind[0] == UNDEFINED:
        return Time([], None)
    if kind[0] == INSTANT:
        return Time([_from_seconds(s) for s in coords], "instant")
    return Time([(_from_seconds(coords[i]), _from_seconds(coords[i + 1]))
                 for i in range(0, len(coords), 2)], "interval")
