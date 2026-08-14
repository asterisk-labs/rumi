"""Pins the file the writer produces, byte for byte.

The IFD writer is moving to C. These tests read the file back with their own
parser, so they check the bytes rather than the code that wrote them.
"""

import hashlib
import struct

import numpy as np
import pytest

import rumi
from rumi import FrameTable
from rumi._write import header_bytes, write_frames

SHORT, LONG, LONG8, DOUBLE, ASCII = 3, 4, 16, 12, 2
TYPE_SIZE = {ASCII: 1, SHORT: 2, LONG: 4, DOUBLE: 8, LONG8: 8}

TILE_OFFSETS, TILE_BYTE_COUNTS = 324, 325

UTM18S = 32718
# The writer reads transform as rasterio's Affine order, (xres, rowrot,
# xorigin, colrot, yres, yorigin), not GDAL's geotransform order.
NORTH_UP = (30.0, 0.0, 500000.0, 0.0, -30.0, 8000000.0)
ROTATED = (30.0, 5.0, 500000.0, 5.0, -30.0, 8000000.0)


def make_frame(shape=(2, 40, 70), tile_size=16, dtype=np.uint16, unit="tile"):
    """A frame whose payloads are all different sizes, so a permutation bug in
    the offset table cannot hide behind equal-length tiles."""
    n = np.prod(shape[1:])
    arr = np.arange(shape[0] * n, dtype=dtype).reshape(shape)
    tf = FrameTable.from_array(arr, tile_size=tile_size, unit=unit)
    tf["compressed"] = [bytes([i % 251]) * (7 + 3 * i) for i in range(len(tf))]
    return tf


def read_ifd(path):
    """Walk the BigTIFF by hand. Returns (entries, next_ifd, blob)."""
    blob = open(path, "rb").read()
    order, version, offset_size, reserved = struct.unpack_from("<HHHH", blob, 0)
    assert (order, version, offset_size, reserved) == (0x4949, 43, 8, 0)
    (first,) = struct.unpack_from("<Q", blob, 8)

    (count,) = struct.unpack_from("<Q", blob, first)
    entries = {}
    pos = first + 8
    for _ in range(count):
        tag, type_, n = struct.unpack_from("<HHQ", blob, pos)
        raw = blob[pos + 12:pos + 20]
        size = TYPE_SIZE[type_] * n
        if size > 8:
            (at,) = struct.unpack_from("<Q", raw)
            payload = blob[at:at + size]
        else:
            at, payload = None, raw[:size]
        entries[tag] = (type_, n, at, payload)
        pos += 20
    (next_ifd,) = struct.unpack_from("<Q", blob, pos)
    return entries, next_ifd, blob


def values(entry, fmt):
    type_, n, _at, payload = entry
    return list(struct.unpack("<" + fmt * n, payload))


def test_bigtiff_header(tmp_path):
    tf = make_frame()
    path = tmp_path / "a.tif"
    write_frames(path, tf["compressed"], tf)
    entries, next_ifd, blob = read_ifd(path)
    assert struct.unpack_from("<Q", blob, 8) == (16,)
    assert next_ifd == 0


def test_tags_ascending(tmp_path):
    tf = make_frame()
    path = tmp_path / "a.tif"
    write_frames(path, tf["compressed"], tf, transform=NORTH_UP, crs=UTM18S)
    tags = list(read_ifd(path)[0])
    assert tags == sorted(tags)


def test_tag_set(tmp_path):
    tf = make_frame()
    path = tmp_path / "a.tif"
    write_frames(path, tf["compressed"], tf)
    entries = read_ifd(path)[0]

    assert set(entries) == {256, 257, 258, 277, 322, 323, 324, 325, 339,
                            34264, 34735}
    assert values(entries[256], "I") == [tf.image_width]
    assert values(entries[257], "I") == [tf.image_length]
    assert values(entries[258], "H") == [16] * tf.bands
    assert values(entries[277], "H") == [tf.bands]
    assert values(entries[322], "H") == [tf.tile_size]
    assert values(entries[323], "H") == [tf.tile_size]
    assert values(entries[339], "H") == [1] * tf.bands

    assert entries[TILE_OFFSETS][0] == LONG8
    assert entries[TILE_BYTE_COUNTS][0] == LONG
    assert entries[TILE_OFFSETS][1] == len(tf)


def test_inline_and_external_values(tmp_path):
    tf = make_frame()
    path = tmp_path / "a.tif"
    write_frames(path, tf["compressed"], tf)
    entries = read_ifd(path)[0]

    for tag, (type_, n, at, _payload) in entries.items():
        inline = TYPE_SIZE[type_] * n <= 8
        assert (at is None) == inline, tag

    external = sorted((at, TYPE_SIZE[t] * n)
                      for t, n, at, _ in entries.values() if at is not None)
    cursor = external[0][0]
    for at, size in external:
        assert at == cursor
        cursor += size + (size & 1)


def test_frame_index_order(tmp_path):
    tf = make_frame()
    path = tmp_path / "a.tif"
    write_frames(path, tf["compressed"], tf)
    entries = read_ifd(path)[0]

    # Both tags run in frame-index order, which is also the physical order, so
    # the byte counts come out exactly as the frames were handed over.
    expected = [len(f) for f in tf["compressed"]]
    assert values(entries[TILE_BYTE_COUNTS], "I") == expected
    assert values(entries[TILE_OFFSETS], "Q") == sorted(
        values(entries[TILE_OFFSETS], "Q"))


def test_payload_offsets(tmp_path):
    tf = make_frame()
    path = tmp_path / "a.tif"
    write_frames(path, tf["compressed"], tf)
    entries, _next, blob = read_ifd(path)

    base = header_bytes(tf)
    offsets = values(entries[TILE_OFFSETS], "Q")
    counts = values(entries[TILE_BYTE_COUNTS], "I")
    assert min(offsets) == base
    assert len(blob) == base + sum(counts)

    for i, (off, n) in enumerate(zip(offsets, counts, strict=True)):
        assert blob[off:off + n] == tf["compressed"][i]


def test_tile_write_order(tmp_path):
    tf = make_frame()
    path = tmp_path / "a.tif"
    write_frames(path, tf["compressed"], tf)
    blob = open(path, "rb").read()
    assert blob[header_bytes(tf):] == b"".join(tf["compressed"])


def test_north_up_transform(tmp_path):
    tf = make_frame()
    path = tmp_path / "a.tif"
    write_frames(path, tf["compressed"], tf, transform=NORTH_UP, crs=UTM18S)
    entries = read_ifd(path)[0]

    # coefficient 0 is the x resolution, coefficient 2 the x origin
    assert 33550 not in entries and 33922 not in entries
    assert values(entries[34264], "d") == [30.0, 0.0, 0.0, 500000.0,
                                           0.0, -30.0, 0.0, 8000000.0,
                                           0.0, 0.0, 0.0, 0.0,
                                           0.0, 0.0, 0.0, 1.0]


def test_rotated_transform(tmp_path):
    tf = make_frame()
    path = tmp_path / "a.tif"
    write_frames(path, tf["compressed"], tf, transform=ROTATED, crs=UTM18S)
    entries = read_ifd(path)[0]

    assert 33550 not in entries and 33922 not in entries
    assert values(entries[34264], "d") == [30.0, 5.0, 0.0, 500000.0,
                                           5.0, -30.0, 0.0, 8000000.0,
                                           0.0, 0.0, 0.0, 0.0,
                                           0.0, 0.0, 0.0, 1.0]


# GeoTIFF key ids
MODEL, RASTER, GEOGRAPHIC, PROJECTED = 1024, 1025, 2048, 3072


def geokeys(path):
    """The key directory as {key id: value}, plus the header."""
    v = values(read_ifd(path)[0][34735], "H")
    return v[:4], {v[4 + i * 4]: v[7 + i * 4] for i in range(v[3])}


def test_geokeys_projected(tmp_path):
    tf = make_frame()
    path = tmp_path / "a.tif"
    write_frames(path, tf["compressed"], tf, transform=NORTH_UP, crs=UTM18S)

    head, keys = geokeys(path)
    assert head == [1, 1, 0, 3]
    assert keys == {MODEL: 1, RASTER: 1, PROJECTED: UTM18S}
    # an EPSG code needs no parameters, so neither companion tag is written
    assert 34736 not in read_ifd(path)[0]
    assert 34737 not in read_ifd(path)[0]


def test_geokeys_geographic(tmp_path):
    tf = make_frame()
    path = tmp_path / "a.tif"
    write_frames(path, tf["compressed"], tf, transform=NORTH_UP, crs=4326)
    assert geokeys(path)[1] == {MODEL: 2, RASTER: 1, GEOGRAPHIC: 4326}


def test_geokeys_kind_is_not_the_code_range(tmp_path):
    """EPSG:4037 sits inside the geographic block and is projected. The kind
    comes from the generated table, not from the number."""
    tf = make_frame()
    path = tmp_path / "a.tif"
    write_frames(path, tf["compressed"], tf, transform=NORTH_UP, crs=4037)
    assert geokeys(path)[1] == {MODEL: 1, RASTER: 1, PROJECTED: 4037}


def test_pixel_is_point(tmp_path):
    tf = make_frame()
    area, point = tmp_path / "area.tif", tmp_path / "point.tif"
    write_frames(area, tf["compressed"], tf, transform=NORTH_UP, crs=UTM18S)
    write_frames(point, tf["compressed"], tf, transform=NORTH_UP, crs=UTM18S,
                 pixel_is_point=True)
    assert geokeys(area)[1][RASTER] == 1
    assert geokeys(point)[1][RASTER] == 2


def test_crs_must_be_an_epsg_code(tmp_path):
    tf = make_frame()
    for bad in ("+proj=utm +zone=18 +south", "WGS 84 / UTM zone 18S", 1.5):
        with pytest.raises((ValueError, TypeError), match="EPSG"):
            write_frames(tmp_path / "a.tif", tf["compressed"], tf,
                         transform=NORTH_UP, crs=bad)


def test_crs_forms_agree(tmp_path):
    tf = make_frame()
    a, b = tmp_path / "int.tif", tmp_path / "str.tif"
    write_frames(a, tf["compressed"], tf, transform=NORTH_UP, crs=UTM18S)
    write_frames(b, tf["compressed"], tf, transform=NORTH_UP, crs="EPSG:32718")
    assert a.read_bytes() == b.read_bytes()


def test_unknown_epsg_is_refused(tmp_path):
    tf = make_frame()
    with pytest.raises(ValueError, match="not a projected or geographic"):
        write_frames(tmp_path / "a.tif", tf["compressed"], tf,
                     transform=NORTH_UP, crs=999999)


def test_write_blob_round_trip(tmp_path):
    tf = make_frame()
    path, blob = rumi.write(tmp_path / "a.tif", tf)

    header = rumi.RumiHeader(blob)
    assert header.shape == (tf.bands, tf.image_length, tf.image_width)
    assert header.dtype == tf.dtype
    d = header.to_dict()
    assert d["tile"] == [tf.tile_size, tf.tile_size]
    assert d["tiles_across"] == tf.tiles_across
    assert d["tiles_down"] == tf.tiles_down
    assert d["base_tiles_offset"] == header_bytes(tf)
    assert rumi.RumiHeader.from_path(path).to_dict() == d


def test_edge_tiles(tmp_path):
    tf = make_frame(shape=(3, 257, 256), tile_size=128)
    assert {t.data.shape for t in tf} == {(128, 128), (1, 128)}
    _path, blob = rumi.write(tmp_path / "a.tif", tf)
    h = rumi.RumiHeader(blob).to_dict()
    assert h["height"] == 257 and h["width"] == 256
    assert h["frames"] == len(tf)
    assert h["frame_unit"] == "tile"


def test_cell_frame_count(tmp_path):
    """A cell frame holds every band, so the grid alone gives the count."""
    tf = make_frame(shape=(3, 257, 256), tile_size=128, unit="cell")
    _path, blob = rumi.write(tmp_path / "a.tif", tf)
    h = rumi.RumiHeader(blob).to_dict()
    assert h["frame_unit"] == "cell"
    assert h["frames"] == len(tf) == h["tiles_across"] * h["tiles_down"]


@pytest.mark.parametrize("dtype", [np.uint8, np.int16, np.uint16, np.int32,
                                   np.float32, np.float64])
def test_sample_format(tmp_path, dtype):
    tf = make_frame(shape=(1, 20, 20), tile_size=16, dtype=dtype)
    path = tmp_path / "a.tif"
    write_frames(path, tf["compressed"], tf)
    entries = read_ifd(path)[0]

    expected = {np.uint8: 1, np.uint16: 1, np.int16: 2, np.int32: 2,
                np.float32: 3, np.float64: 3}[dtype]
    assert values(entries[339], "H") == [expected]
    assert values(entries[258], "H") == [np.dtype(dtype).itemsize * 8]


# Digests of the whole file. The writer is deterministic, so a change here is a
# change to the bytes on disk. Regenerate deliberately, never to make a test
# pass. Last regenerated when the IFD went from 12 tags to 11.
GOLDEN = {
    "plain": "7a6818bd71b3fbb5579ce4c7e07833cd2509555d3cee179ea260697929ad6a11",
    "north_up": "5077418d91f271d316f91fb50859d3693332237e7239a5fb91cdfc314e9aa5e9",
    "rotated": "42afc0fb931b839e9c7f4e821cbd84960482f9d9d1236fd088b59d398948b124",
    "point": "fbbac06cc59555cea0a3454a69b2c43d60d3d070978b29ebce98ce31ab8e4dc9",
}

CASES = {
    "plain": {},
    "north_up": {"transform": NORTH_UP, "crs": UTM18S},
    "rotated": {"transform": ROTATED, "crs": UTM18S},
    "point": {"transform": NORTH_UP, "crs": UTM18S, "pixel_is_point": True},
}


@pytest.mark.parametrize("case", list(CASES))
def test_golden_bytes(tmp_path, case):
    tf = make_frame()
    path = tmp_path / "a.tif"
    write_frames(path, tf["compressed"], tf, **CASES[case])
    digest = hashlib.sha256(open(path, "rb").read()).hexdigest()
    assert digest == GOLDEN[case]


def test_bad_frame_count(tmp_path):
    tf = make_frame()
    with pytest.raises(ValueError, match="expected"):
        write_frames(tmp_path / "a.tif", tf["compressed"][:-1], tf)


def test_empty_payload(tmp_path):
    tf = make_frame()
    frames = list(tf["compressed"])
    frames[3] = b""
    with pytest.raises(ValueError, match="sparse"):
        write_frames(tmp_path / "a.tif", frames, tf)


def test_transform_needs_crs(tmp_path):
    tf = make_frame()
    with pytest.raises(ValueError, match="together"):
        write_frames(tmp_path / "a.tif", tf["compressed"], tf, transform=NORTH_UP)
    with pytest.raises(ValueError, match="together"):
        write_frames(tmp_path / "a.tif", tf["compressed"], tf, crs=UTM18S)


def test_write_needs_all_tiles(tmp_path):
    tf = make_frame()
    tf[2].compressed = None
    with pytest.raises(ValueError, match="no frame"):
        rumi.write(tmp_path / "a.tif", tf)
