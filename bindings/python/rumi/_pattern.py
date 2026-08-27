"""A thin view over the frame pattern C compiles.

The grammar, the layouts rumi defines, and the arithmetic that says where a
frame sits all live in the core, so every binding gets the same answers. What
is here is naming: turning the role codes C reports back into the letters a
pattern is written with.
"""

from typing import NamedTuple

from ._ffi import _check, ffi, lib

# Role codes, as rumi_axis orders them.
_ROLE = ("b", "y", "x", "h", "w")

# The two axes a pattern splits into a grid axis and a tile-local one.
SPATIAL = ("y", "x")
TILE = ("h", "w")

# Axes a frame may hold besides the tile. Time joins this and nothing moves.
AXES = ("b",)
BAND = "b"

# What a table calls an axis. The grid always contributes row and col.
COLUMN = {"b": "band"}
GRID_COLUMNS = ("row", "col")


class PatternError(ValueError):
    """Raised for a pattern rumi cannot honour."""


def _named(codes, ndim):
    return tuple(_ROLE[codes[i]] for i in range(ndim))


def layout_name(frame_unit):
    """The frame's axis order, for a value read back out of a blob."""
    name = lib.rumi_unit_name(frame_unit)
    if name == ffi.NULL:
        raise ValueError(f"unknown frame_unit {frame_unit}")
    return ffi.string(name).decode()


def unit_indexes_bands(frame_unit):
    """True when a frame holds one band, so the index walks bands."""
    layout_name(frame_unit)  # rejects a byte that names no layout
    return bool(lib.rumi_unit_indexes_bands(frame_unit))


def compile_layout_unit(layout):
    """The frame_unit a layout names, such as "b h w"."""
    if not isinstance(layout, str):
        raise PatternError(f"a layout is a string, got {type(layout).__name__}")
    out = ffi.new("uint8_t*")
    try:
        _check(lib.rumi_unit_from_name(layout.encode(), out))
    except ValueError as exc:
        raise PatternError(str(exc)) from None
    return out[0]


class FramePattern:
    """A compiled frame pattern.

    input_axes  roles of the input array's axes, in its own order.
    frame_axes  roles inside one frame, in order.
    frame_unit  the value the blob carries.
    """

    __slots__ = ("text", "input_axes", "frame_axes", "frame_unit")

    def __init__(self, text, compiled):
        self.text = text
        self.frame_unit = compiled.frame_unit
        self.input_axes = _named(compiled.input, compiled.input_ndim)
        self.frame_axes = _named(compiled.frame, compiled.frame_ndim)

    @property
    def bands_are_indexed(self):
        """True when a frame holds one band, so the index walks bands."""
        return BAND not in self.frame_axes

    @property
    def index_columns(self):
        """Canonical names for the axes the frame index walks, outermost
        first. Independent of what the pattern called them."""
        return (*GRID_COLUMNS,
                *(COLUMN[a] for a in AXES if a not in self.frame_axes))

    @property
    def canonical_axes(self):
        """The order a cut arrives in, before the layout permutes it."""
        return (*AXES, *TILE)

    @property
    def canonical_input(self):
        """The input axis order rumi slices in, whatever order it arrived."""
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
    """Where one frame sits and the shape it must arrive in."""

    row: int
    col: int
    band: int
    h: int
    w: int
    dims: tuple[int, ...]


def frame_count(unit, width, length, tile, bands):
    """The grid and the frame count, as (across, down, frames)."""
    a, d, n = ffi.new("uint32_t*"), ffi.new("uint32_t*"), ffi.new("uint64_t*")
    _check(lib.rumi_frame_count(unit, width, length, tile, bands, a, d, n))
    return int(a[0]), int(d[0]), int(n[0])


def frame_at(unit, width, length, tile, bands, index):
    """Everything needed to cut frame `index` out of the caller's array."""
    at = ffi.new("rumi_frame_at*")
    _check(lib.rumi_frame_locate(unit, width, length, tile, bands, index, at))
    return FrameAt(int(at.row), int(at.col), int(at.band), int(at.h), int(at.w),
                   tuple(int(at.dims[i]) for i in range(at.ndim)))
