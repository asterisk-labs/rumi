import struct
from collections.abc import Iterable

from ._dtype import sample_encoding
from ._ffi import _check, ffi, lib

# fixed layout keeps offsets known before writing; packed payloads preserve
# CRS tags returned by C verbatim

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
    # GDAL's north-up pixel scale and tiepoint form
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


def _base_entries(tf):
    B = tf.bands
    sf, bps = sample_encoding(tf.dtype)
    n = tf.tiles_across * tf.tiles_down * B
    return [
        _entry(256, _LONG,  [tf.image_width]),
        _entry(257, _LONG,  [tf.image_length]),
        _entry(258, _SHORT, [bps] * B),
        _entry(259, _SHORT, [_COMPRESSION_OPENZL]),
        _entry(262, _SHORT, [_PHOTOMETRIC_MINISBLACK]),
        _entry(277, _SHORT, [B]),
        _entry(284, _SHORT, [_PLANARCONFIG_SEPARATE]),
        _entry(317, _SHORT, [_PREDICTOR_NONE]),
        _entry(322, _SHORT, [tf.tile_size]),
        _entry(323, _SHORT, [tf.tile_size]),
        (_TILE_OFFSETS, _LONG8, n, None),
        _entry(325, _LONG,  [0] * n),
        _entry(339, _SHORT, [sf] * B),
    ]


def _natural_header(entries):
    cursor = 16 + (8 + 20 * len(entries) + 8)
    for _tag, type_, count, _packed in entries:
        size = _TYPE_SIZE[type_] * count
        if size > 8:
            cursor += size + (size & 1)
    return cursor


def _resolve_base(natural, header_size):
    if header_size == "auto":
        return natural
    if isinstance(header_size, int) and header_size > 0:
        return -(-natural // header_size) * header_size
    raise TypeError("header_size must be 'auto' or a positive int")


def header_bytes(tf, *, transform=None, crs=None,
                 pixel_is_point=False, header_size="auto") -> int:
    """base_tiles_offset for these args, without writing. With a CRS it builds
    the geo tags once to measure them, same as assemble()."""
    entries = _base_entries(tf) + _geo_entries(transform, crs, pixel_is_point)
    return _resolve_base(_natural_header(entries), header_size)


def assemble(path, frames: Iterable[bytes], tf, *,
          transform=None, crs=None, pixel_is_point=False,
          header_size="auto") -> None:
    """Write the file. tf is a TileFrame, and the five fields it carries are
    the header; the frames come in its own order, which is the wire order."""
    frames = [memoryview(f) for f in frames]
    n_tiles = tf.tiles_across * tf.tiles_down * tf.bands
    if len(frames) != n_tiles:
        raise ValueError(f"expected {n_tiles} frames, got {len(frames)}")
    counts = [f.nbytes for f in frames]
    if any(c == 0 for c in counts):
        raise ValueError("empty tile payload, rumi forbids sparse tiles")

    B = tf.bands
    sf, bps = sample_encoding(tf.dtype)

    # rumi tile order is tile-interleaved, with samples innermost
    tpp = tf.tiles_across * tf.tiles_down
    pm_order = [pos * B + b for b in range(B) for pos in range(tpp)]
    counts_pm = [counts[i] for i in pm_order]

    # tag order is ascending; TileOffsets is packed once its base is known
    entries = [
        _entry(256, _LONG,  [tf.image_width]),
        _entry(257, _LONG,  [tf.image_length]),
        _entry(258, _SHORT, [bps] * B),
        _entry(259, _SHORT, [_COMPRESSION_OPENZL]),
        _entry(262, _SHORT, [_PHOTOMETRIC_MINISBLACK]),
        _entry(277, _SHORT, [B]),
        _entry(284, _SHORT, [_PLANARCONFIG_SEPARATE]),
        _entry(317, _SHORT, [_PREDICTOR_NONE]),
        _entry(322, _SHORT, [tf.tile_size]),
        _entry(323, _SHORT, [tf.tile_size]),
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

    natural = cursor
    base_tiles_offset = _resolve_base(natural, header_size)

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

    if len(buf) < base_tiles_offset:
        buf += b"\x00" * (base_tiles_offset - len(buf))
    assert len(buf) == base_tiles_offset
    with open(path, "wb") as fh:
        fh.write(buf)
        for f in frames:
            fh.write(f)