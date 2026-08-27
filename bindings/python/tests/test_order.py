"""Frame order, checked against the C rather than through it.

The Python list order is the physical order of the file, and the C reaches a
frame through frame_index. A disagreement between the two is invisible to every
structural check: the offsets still reconstruct by prefix sum, the run is still
contiguous, the file still validates. It just points at the wrong frame.

So the identity of a frame is taken from the samples it actually holds, never
from the table's own row, col and band, which are derived from the same index
arithmetic that places it. A round trip would pass with two cancelling bugs.

The last test goes further and spells the index out from the specification, so
that a change made consistently on both sides, which a round trip cannot see,
still fails here.
"""

import numpy as np
import pytest
import rumi
from rumi._ffi import _Spec, ffi, lib
from rumi._write import write_frames

PATTERNS = {"tile": "b (row h) (col w) -> row col b (h w)",
            "cell": "b (row h) (col w) -> row col (b h w)",
            "chunky": "b (row h) (col w) -> row col (h w b)"}

SHAPE = (3, 100, 130)
TILE = 32
BANDS, DOWN, ACROSS = 3, 4, 5  # ceil(100/32), ceil(130/32)


def code(band, row, col):
    """A value unique to one band at one grid position."""
    return band * 1000 + row * 100 + col


def scene():
    """Every sample carries the identity of the frame it belongs to."""
    b, y, x = SHAPE
    arr = np.empty(SHAPE, np.uint16)
    for band in range(b):
        for row in range(y):
            for col in range(x):
                arr[band, row, col] = code(band, row // TILE, col // TILE)
    return arr


def payload(i, ident):
    """Distinct in content and in length, so a permutation cannot hide."""
    return ident.to_bytes(2, "little") + bytes([i % 251]) * (5 + 3 * i)


def identify(frame, unit):
    """What the frame holds, read off its samples."""
    return int(frame.data.flat[0] if unit == "tile" else frame.data[0].flat[0])


def build(unit, tmp_path):
    tf = rumi.frames(scene(), PATTERNS[unit], TILE)
    idents = [identify(f, unit) for f in tf]
    tf["compressed"] = [payload(i, ident) for i, ident in enumerate(idents)]
    path = tmp_path / f"{unit}.rumi"
    return tf, idents, path, write_frames(path, tf["compressed"], tf)


def one_range(header, band, row, col):
    """The byte range the C reaches for one pixel of one band in one tile."""
    spec = _Spec(header)
    out = ffi.new("rumi_range**")
    count = ffi.new("size_t*")
    rc = lib.rumi_plan_ranges(spec.handle, ffi.new("int[]", [band + 1]), 1,
                              row * TILE, 1, col * TILE, 1, out, count)
    assert rc == lib.RUMI_OK
    try:
        assert count[0] == 1
        return out[0][0].offset, out[0][0].length
    finally:
        lib.rumi_free(out[0])


@pytest.mark.parametrize("unit", ["tile", "cell", "chunky"])
def test_the_c_reaches_the_frame_that_holds_the_samples(unit, tmp_path):
    """Every grid position and band, against what the file actually stores."""
    _tf, _idents, path, header = build(unit, tmp_path)
    blob = path.read_bytes()

    for band in range(BANDS):
        for row in range(DOWN):
            for col in range(ACROSS):
                offset, length = one_range(header, band, row, col)
                got = int.from_bytes(blob[offset:offset + 2], "little")
                # A cell frame holds every band, so it is named by band 0.
                want = code(band if unit == "tile" else 0, row, col)
                assert got == want, f"band {band} at {row}.{col}"


@pytest.mark.parametrize("unit", ["tile", "cell", "chunky"])
def test_the_list_order_is_the_physical_order(unit, tmp_path):
    """The frames are the payloads concatenated, in the order the table holds
    them, with nothing between."""
    tf, _idents, path, header = build(unit, tmp_path)
    base = rumi.RumiHeader(header).to_dict()["base_frame_offset"]
    blob = path.read_bytes()
    assert blob[base:] == b"".join(tf["compressed"])


@pytest.mark.parametrize("unit", ["tile", "cell", "chunky"])
def test_the_frame_count_matches_the_grid(unit, tmp_path):
    tf, _idents, _path, header = build(unit, tmp_path)
    want = DOWN * ACROSS * (BANDS if unit == "tile" else 1)
    assert len(tf) == want
    assert rumi.RumiHeader(header).frames == want


def spec_frame_index(row, col, band, unit):
    """From the specification, not from the code under test.

    A frame holding one band: all bands at one grid position come before the
    next position. A frame holding every band: one per grid position.
    """
    spatial = row * ACROSS + col
    return spatial * BANDS + band if unit == "tile" else spatial


@pytest.mark.parametrize("unit", ["tile", "cell", "chunky"])
def test_the_index_is_the_one_the_spec_defines(unit, tmp_path):
    """Writer and reader agreeing on a wrong order is invisible to a round
    trip. The offsets are walked here against the spec's own arithmetic."""
    tf, _idents, path, header = build(unit, tmp_path)
    base = rumi.RumiHeader(header).to_dict()["base_frame_offset"]

    # Offsets in file order, as the spec reconstructs them.
    offsets, at = [], base
    for frame in tf["compressed"]:
        offsets.append(at)
        at += len(frame)

    blob = path.read_bytes()
    for band in range(BANDS):
        for row in range(DOWN):
            for col in range(ACROSS):
                want = offsets[spec_frame_index(row, col, band, unit)]
                got, _length = one_range(header, band, row, col)
                assert got == want, f"band {band} at {row}.{col}"
                ident = int.from_bytes(blob[got:got + 2], "little")
                assert ident == code(band if unit == "tile" else 0, row, col)
