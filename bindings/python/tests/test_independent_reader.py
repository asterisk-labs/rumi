"""Cross-check rumi output with a small reader derived from SPEC.md."""

import pathlib
import struct

import numpy as np
import pytest
import rumi

geozl = pytest.importorskip("geozl")

T, B, Y, X = 4, 3, 40, 40
TILE = 16

SHORT, LONG, DOUBLE, LONG8 = 3, 4, 12, 16
FIELD_TYPES = {
    SHORT: ("H", 2),
    LONG: ("I", 4),
    DOUBLE: ("d", 8),
    LONG8: ("Q", 8),
}

IMAGE_WIDTH, IMAGE_LENGTH = 256, 257
BITS_PER_SAMPLE, SAMPLES_PER_PIXEL = 258, 277
TILE_WIDTH, TILE_LENGTH = 322, 323
TILE_OFFSETS, TILE_BYTE_COUNTS = 324, 325
SAMPLE_FORMAT, FRAME_UNIT, TIME_COUNT = 339, 65000, 65001

SAMPLE_DTYPE = {(1, 8): "u1", (1, 16): "<u2", (2, 16): "<i2", (3, 32): "<f4"}

# "s" is the h/w pair. Units 1 and 2 use "?" for their only non-spatial axis.
FRAME_AXES = {0: "s", 1: "?s", 2: "s?", 3: "bts", 4: "tbs",
              5: "bst", 6: "tsb", 7: "sbt", 8: "stb", 9: "s"}

CUBE_LAYOUTS = {
    0: "t b (row h) (col w) -> row col b t (h w)",
    3: "t b (row h) (col w) -> row col (b t h w)",
    4: "t b (row h) (col w) -> row col (t b h w)",
    5: "t b (row h) (col w) -> row col (b h w t)",
    6: "t b (row h) (col w) -> row col (t h w b)",
    7: "t b (row h) (col w) -> row col (h w b t)",
    8: "t b (row h) (col w) -> row col (h w t b)",
    9: "t b (row h) (col w) -> row col t b (h w)",
}

IMAGE_LAYOUTS = {
    0: "b (row h) (col w) -> row col b (h w)",
    1: "b (row h) (col w) -> row col (b h w)",
    2: "b (row h) (col w) -> row col (h w b)",
}


def entries_of(blob):
    magic, version, reserved, at = struct.unpack_from("<4sHHQ", blob, 0)
    assert (magic, version, reserved, at) == (b"RUMI", 1, 0, 16)

    (count,) = struct.unpack_from("<Q", blob, at)
    assert count == 13
    assert struct.unpack_from("<Q", blob, at + 8 + 20 * count) == (0,)

    for i in range(count):
        e = at + 8 + 20 * i
        tag, kind, n = struct.unpack_from("<HHQ", blob, e)
        yield tag, kind, n, e


def read_ifd(blob):
    ifd = {}
    for tag, kind, count, e in entries_of(blob):
        _code, width = FIELD_TYPES[kind]
        size = width * count
        if size <= 8:
            raw = blob[e + 12:e + 12 + size]
        else:
            (where,) = struct.unpack_from("<Q", blob, e + 12)
            raw = blob[where:where + size]
        ifd[tag] = (kind, count, raw)
    return ifd


def field(ifd, tag):
    kind, count, raw = ifd[tag]
    code, _width = FIELD_TYPES[kind]
    return list(struct.unpack("<" + code * count, raw))


def cell_axes(unit, bands):
    order = FRAME_AXES[unit].replace("?", "b" if bands > 1 else "t")
    axes = []
    for a in order:
        axes.extend(["h", "w"] if a == "s" else [a])
    return axes


def cell_to_tbhw(cell, unit, bands, steps, h, w):
    axes = cell_axes(unit, bands)
    extent = {"b": bands, "t": steps, "h": h, "w": w}
    cell = cell.reshape([extent[a] for a in axes])
    for a in "tb":
        if a not in axes:
            cell, axes = cell[np.newaxis], [a] + axes
    return cell.transpose([axes.index(a) for a in "tbhw"])


def decode(blob, offset, count, dtype, shape):
    frame = geozl.decompress(blob[offset:offset + count])
    assert len(frame) == int(np.prod(shape)) * dtype.itemsize
    return np.frombuffer(frame, dtype).reshape(shape)


def spec_read(path):
    """Return every sample the file holds, as (T, B, image_length, width)."""
    blob = pathlib.Path(path).read_bytes()
    ifd = read_ifd(blob)

    (width,), (length,) = field(ifd, IMAGE_WIDTH), field(ifd, IMAGE_LENGTH)
    (bands,), (steps,) = field(ifd, SAMPLES_PER_PIXEL), field(ifd, TIME_COUNT)
    (tile_w,), (tile_h,) = field(ifd, TILE_WIDTH), field(ifd, TILE_LENGTH)
    (unit,) = field(ifd, FRAME_UNIT)
    offsets, counts = field(ifd, TILE_OFFSETS), field(ifd, TILE_BYTE_COUNTS)

    bits, fmt = field(ifd, BITS_PER_SAMPLE), field(ifd, SAMPLE_FORMAT)
    assert bits == [bits[0]] * bands and fmt == [fmt[0]] * bands
    dtype = np.dtype(SAMPLE_DTYPE[fmt[0], bits[0]])

    across, down = -(-width // tile_w), -(-length // tile_h)
    holds_tile = unit in (0, 9)
    assert len(offsets) == len(counts) == \
        across * down * (bands * steps if holds_tile else 1)

    out = np.empty((steps, bands, length, width), dtype)
    for row in range(down):
        for col in range(across):
            top, left = row * tile_h, col * tile_w
            h, w = min(tile_h, length - top), min(tile_w, width - left)
            spatial = row * across + col
            if holds_tile:
                for b in range(bands):
                    for t in range(steps):
                        i = frame_index(unit, spatial, b, t, bands, steps)
                        out[t, b, top:top + h, left:left + w] = decode(
                            blob, offsets[i], counts[i], dtype, (h, w))
            else:
                cell = decode(blob, offsets[spatial], counts[spatial], dtype,
                              (bands * steps * h * w,))
                out[:, :, top:top + h, left:left + w] = cell_to_tbhw(
                    cell, unit, bands, steps, h, w)
    return out


def frame_index(unit, spatial, band, step, bands, steps):
    if unit == 0:
        return (spatial * bands + band) * steps + step
    return (spatial * steps + step) * bands + band


def array_at(blob, tag):
    for found, kind, count, e in entries_of(blob):
        if found == tag:
            code, width = FIELD_TYPES[kind]
            assert width * count > 8, f"tag {tag} is inline"
            (where,) = struct.unpack_from("<Q", blob, e + 12)
            return where, code
    raise AssertionError(f"tag {tag} is not in the IFD")


def swap_frames(blob, i, j):
    out = bytearray(blob)
    for tag in (TILE_OFFSETS, TILE_BYTE_COUNTS):
        where, code = array_at(blob, tag)
        size = struct.calcsize("<" + code)
        a, b = where + i * size, where + j * size
        out[a:a + size], out[b:b + size] = out[b:b + size], out[a:a + size]
    return bytes(out)


def unit_of(path):
    (unit,) = field(read_ifd(pathlib.Path(path).read_bytes()), FRAME_UNIT)
    return unit


DATES = ["2024-05-01", "2024-06-01", "2024-07-01", "2024-08-01"]


def store(tmp_path, pattern, arr, **kw):
    tf = rumi.frames(arr, pattern, TILE)
    graphs = {}
    for frame in tf:
        graph = graphs.get(frame.data.shape)
        if graph is None:
            graph = graphs[frame.data.shape] = geozl.graph(
                frame.data, "planar>zigzag>zstd")
        frame.compressed = geozl.compress(frame.data, graph=graph)
    return rumi.write(tmp_path / "a.rumi", tf, **kw)


def cube(dtype="uint16"):
    return (np.arange(T * B * Y * X) % 4000).astype(dtype).reshape(T, B, Y, X)


def image(dtype="uint16"):
    return (np.arange(B * Y * X) % 4000).astype(dtype).reshape(B, Y, X)


@pytest.mark.parametrize("unit", list(CUBE_LAYOUTS))
def test_a_cube_reads_the_same_both_ways(unit, tmp_path):
    data = cube()
    path, header = store(tmp_path, CUBE_LAYOUTS[unit], data, time=DATES)
    assert unit_of(path) == unit
    got = np.asarray(rumi.read(path, header))
    assert np.array_equal(spec_read(path), got.reshape(T, B, Y, X))
    assert np.array_equal(got, data)


@pytest.mark.parametrize("unit", list(IMAGE_LAYOUTS))
def test_an_image_reads_the_same_both_ways(unit, tmp_path):
    data = image()
    path, header = store(tmp_path, IMAGE_LAYOUTS[unit], data)
    assert unit_of(path) == unit
    got = np.asarray(rumi.read(path, header))
    assert np.array_equal(spec_read(path), got.reshape(1, B, Y, X))
    assert np.array_equal(got, data)


@pytest.mark.parametrize("dtype", ["uint8", "uint16", "int16", "float32"])
def test_the_file_says_how_to_read_its_samples(dtype, tmp_path):
    data = image(dtype)
    path, header = store(tmp_path, IMAGE_LAYOUTS[1], data)
    independent = spec_read(path)
    assert independent.dtype == np.dtype(dtype)
    assert np.array_equal(independent, np.asarray(rumi.read(path, header))[np.newaxis])


def test_the_reader_follows_the_offsets_the_file_carries(tmp_path):
    """TileOffsets controls a foreign reader; rumi rejects noncanonical order."""
    path, _header = store(tmp_path, CUBE_LAYOUTS[0], cube(), time=DATES)
    blob = path.read_bytes()
    before = spec_read(path)

    first = frame_index(0, 0, 0, 0, B, T)
    second = frame_index(0, 1, 0, 0, B, T)

    moved = tmp_path / "moved.rumi"
    moved.write_bytes(swap_frames(blob, first, second))
    after = spec_read(moved)

    assert np.array_equal(after[0, 0, :TILE, :TILE],
                          before[0, 0, :TILE, TILE:2 * TILE])
    assert np.array_equal(after[0, 0, :TILE, TILE:2 * TILE],
                          before[0, 0, :TILE, :TILE])

    with pytest.raises((ValueError, IOError)):
        rumi.info(source=moved)
