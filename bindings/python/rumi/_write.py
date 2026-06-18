import struct
from collections.abc import Iterable
from dataclasses import dataclass

import numpy as np

from ._dtype import sample_encoding


@dataclass(frozen=True)
class Layout:
    """Grid and dtype of a tiled image. The header blob minus the bits the
    writer fills in, base_tiles_offset and the per-tile byte counts."""

    image_width: int
    image_length: int
    tile_width: int
    tile_length: int
    samples_per_pixel: int
    dtype: np.dtype

    @property
    def sample_format(self) -> int:
        return sample_encoding(self.dtype)[0]

    @property
    def bits_per_sample(self) -> int:
        return sample_encoding(self.dtype)[1]

    @property
    def tiles_across(self) -> int:
        return -(-self.image_width // self.tile_width)

    @property
    def tiles_down(self) -> int:
        return -(-self.image_length // self.tile_length)

    @property
    def n_tiles(self) -> int:
        return self.tiles_across * self.tiles_down * self.samples_per_pixel


def tile(arr: np.ndarray, tile: int = 512) -> tuple[np.ndarray, Layout]:
    if arr.ndim != 3:
        raise ValueError(f"expected (B, Y, X), got shape {arr.shape}")
    if tile < 16 or tile % 16:
        raise ValueError(f"tile must be a positive multiple of 16, got {tile}")
    sample_encoding(arr.dtype)  # reject unsupported dtype early

    B, Y, X = arr.shape
    T = tile
    across = -(-X // T)
    down = -(-Y // T)

    pad_y, pad_x = down * T - Y, across * T - X
    if pad_y or pad_x:
        arr = np.pad(arr, ((0, 0), (0, pad_y), (0, pad_x)))

    # split Y into (down, T) and X into (across, T), then put samples innermost
    g = arr.reshape(B, down, T, across, T).transpose(1, 3, 0, 2, 4)
    chunks = g.reshape(down * across * B, T, T)

    return chunks, Layout(X, Y, T, T, B, arr.dtype)


# BigTIFF assembler. The layout is fixed so every offset is known before a byte
# is written: header, then the single IFD and its out-of-line arrays, then the
# tile payloads in tile order with nothing between them. Every entry carries its
# packed payload, so the CRS tags that come from C embed verbatim.

_LE = "<"
_SHORT, _LONG, _LONG8, _DOUBLE, _ASCII = 3, 4, 16, 12, 2
_TYPE_FMT = {_SHORT: "H", _LONG: "I", _LONG8: "Q", _DOUBLE: "d"}
_TYPE_SIZE = {_SHORT: 2, _LONG: 4, _LONG8: 8, _DOUBLE: 8, _ASCII: 1}

_COMPRESSION_OPENZL = 60000
_PHOTOMETRIC_MINISBLACK = 1
_PLANARCONFIG_SEPARATE = 2
_PREDICTOR_NONE = 1

_TILE_OFFSETS = 324


def _pack(type_, values):
    return struct.pack(_LE + _TYPE_FMT[type_] * len(values), *values)


def _entry(tag, type_, values):
    return (tag, type_, len(values), _pack(type_, values))


def _crs_geokeys(crs, pixel_is_point):
    from ._ffi import _check, ffi, lib

    s = (f"EPSG:{crs}" if isinstance(crs, int) else str(crs)).encode("utf-8")
    out = [(ffi.new("unsigned char**"), ffi.new("size_t*")) for _ in range(3)]
    _check(lib.rumi_geokeys(
        s, 1 if pixel_is_point else 0,
        out[0][0], out[0][1], out[1][0], out[1][1], out[2][0], out[2][1]))
    try:
        return tuple(bytes(ffi.buffer(p[0], n[0])) if n[0] else b""
                     for p, n in out)
    finally:
        for p, n in out:
            if n[0]:
                lib.rumi_free(p[0])


def _geo_entries(transform, crs, pixel_is_point):
    if transform is None and crs is None:
        return []
    if (transform is None) != (crs is None):
        raise ValueError("transform and crs must be given together")

    a, b, c, d, e, f = (float(v) for v in tuple(transform)[:6])
    out = []
    # north up, no rotation: pixel scale + tiepoint, exactly what GDAL emits.
    if b == 0.0 and d == 0.0:
        out.append(_entry(33550, _DOUBLE, [a, -e, 0.0]))
        out.append(_entry(33922, _DOUBLE, [0.0, 0.0, 0.0, c, f, 0.0]))
    else:
        out.append(_entry(34264, _DOUBLE, [a, b, 0.0, c,
                                           d, e, 0.0, f,
                                           0.0, 0.0, 0.0, 0.0,
                                           0.0, 0.0, 0.0, 1.0]))

    dir_b, dbl_b, ascii_b = _crs_geokeys(crs, pixel_is_point)
    out.append((34735, _SHORT, len(dir_b) // 2, dir_b))
    if dbl_b:
        out.append((34736, _DOUBLE, len(dbl_b) // 8, dbl_b))
    if ascii_b:
        out.append((34737, _ASCII, len(ascii_b), ascii_b))
    return out


def write(path, frames: Iterable[bytes], layout: Layout, *,
          transform=None, crs=None, pixel_is_point=False) -> None:
    frames = [memoryview(f) for f in frames]
    if len(frames) != layout.n_tiles:
        raise ValueError(f"expected {layout.n_tiles} frames, got {len(frames)}")
    counts = [f.nbytes for f in frames]
    if any(c == 0 for c in counts):
        raise ValueError("empty tile payload, rumi forbids sparse tiles")

    B = layout.samples_per_pixel
    sf, bps = layout.sample_format, layout.bits_per_sample

    # rumi lays the tile bytes out tile-interleaved (samples innermost).
    tpp = layout.tiles_across * layout.tiles_down
    pm_order = [pos * B + b for b in range(B) for pos in range(tpp)]
    counts_pm = [counts[i] for i in pm_order]

    # (tag, type, count, packed) in ascending tag order. TileOffsets packs once
    # the data offset is known. Geo tags, if any, append above 339.
    entries = [
        _entry(256, _LONG,  [layout.image_width]),
        _entry(257, _LONG,  [layout.image_length]),
        _entry(258, _SHORT, [bps] * B),
        _entry(259, _SHORT, [_COMPRESSION_OPENZL]),
        _entry(262, _SHORT, [_PHOTOMETRIC_MINISBLACK]),
        _entry(277, _SHORT, [B]),
        _entry(284, _SHORT, [_PLANARCONFIG_SEPARATE]),
        _entry(317, _SHORT, [_PREDICTOR_NONE]),
        _entry(322, _SHORT, [layout.tile_width]),
        _entry(323, _SHORT, [layout.tile_length]),
        (_TILE_OFFSETS, _LONG8, len(frames), None),
        _entry(325, _LONG,  counts_pm),
        _entry(339, _SHORT, [sf] * B),
    ]
    entries += _geo_entries(transform, crs, pixel_is_point)

    ifd_offset = 16
    ifd_size = 8 + 20 * len(entries) + 8
    cursor = ifd_offset + ifd_size

    ext_offset = {}
    for tag, type_, count, _ in entries:
        size = _TYPE_SIZE[type_] * count
        if size > 8:
            ext_offset[tag] = cursor
            cursor += size + (size & 1)
    base_tiles_offset = cursor

    off = base_tiles_offset
    tile_offsets = []
    for c in counts:
        tile_offsets.append(off)
        off += c
    offsets_pm = [tile_offsets[i] for i in pm_order]

    buf = bytearray()
    buf += struct.pack(_LE + "HHHH", 0x4949, 43, 8, 0)
    buf += struct.pack(_LE + "Q", ifd_offset)
    buf += struct.pack(_LE + "Q", len(entries))

    ext = bytearray()
    for tag, type_, count, packed in entries:
        if tag == _TILE_OFFSETS:
            packed = _pack(_LONG8, offsets_pm)
        buf += struct.pack(_LE + "HHQ", tag, type_, count)
        if len(packed) <= 8:
            buf += packed.ljust(8, b"\x00")
        else:
            buf += struct.pack(_LE + "Q", ext_offset[tag])
            ext += packed
            if len(packed) & 1:
                ext += b"\x00"
    buf += struct.pack(_LE + "Q", 0)  # one IFD, no next
    buf += ext

    assert len(buf) == base_tiles_offset
    with open(path, "wb") as fh:
        fh.write(buf)
        for f in frames:
            fh.write(f)