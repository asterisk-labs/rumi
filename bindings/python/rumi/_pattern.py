from typing import NamedTuple

from ._ffi import _check, ffi, lib

# Read axis names from the core so the binding cannot drift from rumi_axis.
_ROLE = tuple(ffi.string(lib.rumi_axis_name(i)).decode() for i in range(6))

# The two axes a pattern splits into a grid axis and a tile-local one.
SPATIAL = ("y", "x")
TILE = ("h", "w")

# Non-spatial axes that a frame may contain.
AXES = ("b", "t")

# FrameTable column names. Every table also has row and col.
COLUMN = {"b": "band", "t": "time"}
GRID_COLUMNS = ("row", "col")


class PatternError(ValueError):
    """Raised for a pattern rumi cannot honour."""


def _named(codes, ndim):
    return tuple(_ROLE[codes[i]] for i in range(ndim))


def layout_name(frame_unit, bands, times):
    """Return the decoded axis order for a frame unit."""
    out = ffi.new("char[16]")
    try:
        _check(lib.rumi_unit_name(frame_unit, bands, times, out, 16))
    except ValueError as exc:
        raise ValueError(f"unknown frame_unit {frame_unit}: {exc}") from None
    return ffi.string(out).decode()


def index_axes(frame_unit, bands, times):
    """Return indexed axes from outermost to innermost."""
    buf = ffi.new("uint8_t[2]")
    ndim = ffi.new("int*")
    _check(lib.rumi_unit_index_axes(frame_unit, bands, times, buf, ndim))
    return _named(buf, ndim[0])


def unit_indexes_bands(frame_unit, bands, times):
    """Return whether band is part of the frame index."""
    layout_name(frame_unit, bands, times)  # Validate the unit first.
    return bool(lib.rumi_unit_indexes_bands(frame_unit, bands, times))


def compile_layout_unit(layout, bands, times):
    """Return the frame unit for an axis order such as ``"b h w"``."""
    if not isinstance(layout, str):
        raise PatternError(f"a layout is a string, got {type(layout).__name__}")
    out = ffi.new("uint8_t*")
    try:
        _check(lib.rumi_unit_from_name(layout.encode(), bands, times, out))
    except ValueError as exc:
        raise PatternError(str(exc)) from None
    return out[0]


class FramePattern:
    """A compiled frame pattern.

    input_axes  roles of the input array's axes, in its own order.
    frame_axes  roles inside one frame, in order.
    index_axes  roles the frame index walks, outermost first.

    The frame unit also depends on the band and time extents because axes of
    length one are omitted from the recorded layout.
    """

    __slots__ = ("text", "input_axes", "frame_axes", "index_axes", "_c")

    def __init__(self, text, compiled):
        self.text = text
        self.input_axes = _named(compiled.input, compiled.input_ndim)
        self.frame_axes = _named(compiled.frame, compiled.frame_ndim)
        self.index_axes = _named(compiled.index, compiled.index_ndim)
        self._c = compiled

    def frame_unit(self, bands, times):
        """Return the frame unit recorded for the given extents."""
        out = ffi.new("uint8_t*")
        try:
            _check(lib.rumi_frame_unit(self._c, bands, times, out))
        except ValueError as exc:
            raise PatternError(str(exc)) from None
        return out[0]

    def holds(self, axis):
        """Return whether the decoded frame contains ``axis``."""
        return axis in self.frame_axes

    def index_columns(self, bands, times):
        """Return canonical index column names, outermost first."""
        return (*GRID_COLUMNS,
                *(COLUMN[a] for a in self.index_axes
                  if (bands if a == "b" else times) > 1))

    @property
    def canonical_axes(self):
        """Return the axis order used before applying the frame permutation."""
        return (*AXES, *TILE)

    @property
    def canonical_input(self):
        """Return the canonical input axis order."""
        return (*AXES, *SPATIAL)

    def __repr__(self):
        return f"<rumi.FramePattern {self.text!r}>"

    def __str__(self):
        return " ".join(self.frame_axes)


def compile_pattern(text):
    """Compile a frame pattern, or raise PatternError."""
    if not isinstance(text, str):
        raise PatternError(f"a pattern is a string, got {type(text).__name__}")
    out = ffi.new("rumi_frame_pattern*")
    try:
        _check(lib.rumi_compile_frame_pattern(text.encode(), out))
    except ValueError as exc:
        raise PatternError(str(exc)) from None
    return FramePattern(text.strip(), out)


class FrameAt(NamedTuple):
    """Position, shape, and permutation for one frame."""

    row: int
    col: int
    band: int
    time: int
    h: int
    w: int
    dims: tuple[int, ...]
    perm: tuple[int, ...]


def frame_count(unit, width, length, tile, bands, times):
    """Return ``(tiles_across, tiles_down, frame_count)``."""
    a, d, n = ffi.new("uint32_t*"), ffi.new("uint32_t*"), ffi.new("uint64_t*")
    _check(lib.rumi_frame_count(unit, width, length, tile, bands, times, a, d, n))
    return int(a[0]), int(d[0]), int(n[0])


def frame_at(unit, width, length, tile, bands, times, index):
    """Return the position, shape, and permutation of ``index``."""
    at = ffi.new("rumi_frame_at*")
    _check(lib.rumi_frame_locate(unit, width, length, tile, bands, times,
                                 index, at))
    return FrameAt(int(at.row), int(at.col), int(at.band), int(at.time),
                   int(at.h), int(at.w),
                   tuple(int(at.dims[i]) for i in range(at.ndim)),
                   tuple(int(at.perm[i]) for i in range(at.ndim)))
