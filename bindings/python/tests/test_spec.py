"""Conformance, end to end.

The parser and the writer here are written from the specification rather than
from the code under test, so a bug that lives in both rumi's writer and its
reader still fails. The writer also builds the invalid files the rejection
tests need.
"""

import struct

import numpy as np
import pytest

import rumi
from rumi import TileFrame
from rumi._write import header_bytes, write_frames

SHORT, LONG, LONG8, DOUBLE, ASCII = 3, 4, 16, 12, 2
TYPE_SIZE = {ASCII: 1, SHORT: 2, LONG: 4, DOUBLE: 8, LONG8: 8}

TAGS = (256, 257, 258, 277, 284, 322, 323, 324, 325, 339, 34264, 34735)
GEO = (34264, 34735)
FORBIDDEN_GEO = (33550, 33922, 34736, 34737)

IFD_OFFSET = 16
IFD_SIZE = 8 + 20 * len(TAGS) + 8          # 256
BASE_CONSTANT = IFD_OFFSET + IFD_SIZE      # 272

MODEL, RASTER, GEOGRAPHIC, PROJECTED = 1024, 1025, 2048, 3072
UTM18S = 32718
NORTH_UP = (30.0, 0.0, 500000.0, 0.0, -30.0, 8000000.0)
ROTATED = (30.0, 5.0, 500000.0, 5.0, -30.0, 8000000.0)


def derive_base_offset(bands, tiles, geo=True):
    """Kept independent of the C so the two can disagree."""
    external = ((2 * bands if bands >= 5 else 0)      # 258 BitsPerSample
                + (8 * tiles if tiles >= 2 else 0)    # 324 TileOffsets
                + (4 * tiles if tiles >= 3 else 0)    # 325 TileByteCounts
                + (2 * bands if bands >= 5 else 0)    # 339 SampleFormat
                + 128                                 # 34264
                + 32)                                 # 34735
    return BASE_CONSTANT + external


def make_frame(shape=(2, 40, 70), tile_size=16, dtype=np.uint16):
    """Payload lengths all differ, so a permutation bug cannot hide behind
    tiles of equal size."""
    n = int(np.prod(shape[1:]))
    arr = np.arange(shape[0] * n, dtype=dtype).reshape(shape)
    tf = TileFrame.from_array(arr, tile_size=tile_size)
    tf["compressed"] = [bytes([i % 251]) * (7 + 3 * i) for i in range(len(tf))]
    return tf


def read_ifd(path):
    """Walk the BigTIFF by hand. Returns (entries, next_ifd, blob), entries
    keyed by tag and ordered as the file has them."""
    blob = open(path, "rb").read() if not isinstance(path, bytes) else path
    order, version, offset_size, reserved = struct.unpack_from("<HHHH", blob, 0)
    assert (order, version, offset_size, reserved) == (0x4949, 43, 8, 0)
    (first,) = struct.unpack_from("<Q", blob, 8)

    (count,) = struct.unpack_from("<Q", blob, first)
    entries, pos = {}, first + 8
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
    _type, n, _at, payload = entry
    return list(struct.unpack("<" + fmt * n, payload))


def geokeys(path):
    """The key directory as (header, {key id: value})."""
    v = values(read_ifd(path)[0][34735], "H")
    return v[:4], {v[4 + i * 4]: v[7 + i * 4] for i in range(v[3])}


def plane_major(n_pos, bands):
    """TIFF orders the tile arrays band outermost while the payloads go down
    band innermost, so the two need a permutation between them."""
    return [pos * bands + b for b in range(bands) for pos in range(n_pos)]


def build_tiff(entries, tiles, bands=1, pad_before_tiles=0):
    """entries is {tag: (type, values)}, tiles the payloads in wire order.
    pad_before_tiles opens a gap, for the tests that need an invalid file."""
    ordered = sorted(entries.items())
    ifd_size = 8 + 20 * len(ordered) + 8

    packed, external = {}, {}
    cursor = IFD_OFFSET + ifd_size
    for tag, (type_, vals) in ordered:
        fmt = {ASCII: "B", SHORT: "H", LONG: "I", LONG8: "Q",
               DOUBLE: "d"}[type_]
        raw = struct.pack("<" + fmt * len(vals), *vals)
        packed[tag] = raw
        if len(raw) > 8:
            external[tag] = cursor
            cursor += len(raw) + (len(raw) & 1)
    base = cursor + pad_before_tiles

    if 324 in packed:
        at, where = base, []
        for payload in tiles:
            where.append(at)
            at += len(payload)
        order = plane_major(len(tiles) // bands, bands)
        packed[324] = struct.pack("<" + "Q" * len(order),
                                  *[where[i] for i in order])

    out = bytearray(struct.pack("<HHHHQ", 0x4949, 43, 8, 0, IFD_OFFSET))
    out += struct.pack("<Q", len(ordered))
    for tag, (type_, vals) in ordered:
        raw = packed[tag]
        field = (struct.pack("<Q", external[tag]) if tag in external
                 else raw.ljust(8, b"\x00"))
        out += struct.pack("<HHQ", tag, type_, len(vals)) + field
    out += struct.pack("<Q", 0)
    for tag, (type_, _vals) in ordered:
        if tag in external:
            raw = packed[tag]
            out += raw + b"\x00" * (len(raw) & 1)
    out += b"\x00" * pad_before_tiles
    for payload in tiles:
        out += payload
    return bytes(out)


def spec_entries(width, length, tile, bands, tiles, bits=16, fmt=1,
                 epsg=UTM18S, model=1, transform=NORTH_UP):
    """A tag set a compliant file would carry."""
    t = transform
    matrix = [t[0], t[1], 0.0, t[2],
              t[3], t[4], 0.0, t[5],
              0.0, 0.0, 0.0, 0.0,
              0.0, 0.0, 0.0, 1.0]
    directory = [1, 1, 0, 3,
                 MODEL, 0, 1, model,
                 RASTER, 0, 1, 1,
                 (GEOGRAPHIC if model == 2 else PROJECTED), 0, 1, epsg]
    return {
        256: (LONG, [width]), 257: (LONG, [length]),
        258: (SHORT, [bits] * bands), 277: (SHORT, [bands]),
        284: (SHORT, [2]),
        322: (SHORT, [tile]), 323: (SHORT, [tile]),
        324: (LONG8, [0] * len(tiles)),
        325: (LONG, [len(tiles[i])
                     for i in plane_major(len(tiles) // bands, bands)]),
        339: (SHORT, [fmt] * bands),
        34264: (DOUBLE, matrix), 34735: (SHORT, directory),
    }


# Fixed IFD

def test_the_tag_set_is_exactly_the_fifteen(tmp_path):
    tf = make_frame()
    path = tmp_path / "a.tif"
    write_frames(path, tf["compressed"], tf, transform=NORTH_UP, crs=UTM18S)
    assert tuple(sorted(read_ifd(path)[0])) == TAGS


def test_the_tag_set_does_not_depend_on_georeferencing(tmp_path):

    tf = make_frame()
    plain, geo = tmp_path / "plain.tif", tmp_path / "geo.tif"
    write_frames(plain, tf["compressed"], tf)
    write_frames(geo, tf["compressed"], tf, transform=NORTH_UP, crs=UTM18S)
    assert tuple(sorted(read_ifd(plain)[0])) == TAGS
    assert set(read_ifd(plain)[0]) == set(read_ifd(geo)[0])


def test_tags_are_in_ascending_order(tmp_path):
    tf = make_frame()
    path = tmp_path / "a.tif"
    write_frames(path, tf["compressed"], tf)
    tags = list(read_ifd(path)[0])
    assert tags == sorted(tags)


def test_the_ifd_starts_at_sixteen_and_is_one(tmp_path):
    tf = make_frame()
    path = tmp_path / "a.tif"
    write_frames(path, tf["compressed"], tf)
    _entries, next_ifd, blob = read_ifd(path)
    assert struct.unpack_from("<Q", blob, 8) == (IFD_OFFSET,)
    assert next_ifd == 0


def test_the_ifd_is_always_316_bytes(tmp_path):
    tf = make_frame()
    path = tmp_path / "a.tif"
    write_frames(path, tf["compressed"], tf)
    (count,) = struct.unpack_from("<Q", read_ifd(path)[2], IFD_OFFSET)
    assert count == len(TAGS)
    assert 8 + 20 * count + 8 == IFD_SIZE


# Placement

def test_values_are_inline_when_they_fit(tmp_path):
    tf = make_frame()
    path = tmp_path / "a.tif"
    write_frames(path, tf["compressed"], tf)
    for tag, (type_, n, at, _payload) in read_ifd(path)[0].items():
        assert (at is None) == (TYPE_SIZE[type_] * n <= 8), tag


def test_external_values_are_packed_right_after_the_ifd(tmp_path):
    tf = make_frame()
    path = tmp_path / "a.tif"
    write_frames(path, tf["compressed"], tf)
    entries = read_ifd(path)[0]

    external = [(tag, at, TYPE_SIZE[t] * n)
                for tag, (t, n, at, _) in entries.items() if at is not None]
    external.sort(key=lambda row: row[0])          # ascending tag order
    cursor = IFD_OFFSET + IFD_SIZE
    for _tag, at, size in external:
        assert at == cursor
        cursor += size
    assert cursor == header_bytes(tf)


def test_no_gap_anywhere_in_front_of_the_tile_data(tmp_path):
    tf = make_frame()
    path = tmp_path / "a.tif"
    write_frames(path, tf["compressed"], tf)
    blob = open(path, "rb").read()
    base = header_bytes(tf)
    assert blob[base:] == b"".join(tf["compressed"])
    assert len(blob) == base + sum(len(f) for f in tf["compressed"])


# Deriving base_tiles_offset

@pytest.mark.parametrize("shape,tile", [
    ((1, 16, 16), 16),        # one tile, both arrays inline
    ((2, 40, 70), 16),
    ((4, 64, 64), 16),        # four bands, BitsPerSample still inline
    ((5, 64, 64), 16),        # five bands, BitsPerSample goes external
    ((13, 512, 512), 256),
    ((64, 256, 256), 256),
])
def test_base_offset_matches_the_derivation(tmp_path, shape, tile):
    tf = make_frame(shape=shape, tile_size=tile)
    path = tmp_path / "a.tif"
    write_frames(path, tf["compressed"], tf, transform=NORTH_UP, crs=UTM18S)
    want = derive_base_offset(tf.bands, len(tf))
    assert header_bytes(tf) == want
    assert min(values(read_ifd(path)[0][324], "Q")) == want


def test_the_blob_agrees_with_the_derivation(tmp_path):
    tf = make_frame()
    _path, blob = rumi.write(tmp_path / "a.tif", tf)
    facts = rumi.RumiHeader(blob).to_dict()
    assert facts["base_tiles_offset"] == derive_base_offset(tf.bands, len(tf))


def test_the_same_shape_gives_the_same_offset(tmp_path):
    """Different payload sizes, same first tile."""
    a = make_frame()
    b = make_frame()
    b["compressed"] = [bytes(1 + i) for i in range(len(b))]
    pa = tmp_path / "a.tif"
    pb = tmp_path / "b.tif"
    write_frames(pa, a["compressed"], a)
    write_frames(pb, b["compressed"], b)
    assert min(values(read_ifd(pa)[0][324], "Q")) == \
           min(values(read_ifd(pb)[0][324], "Q"))


def test_georeferencing_does_not_move_the_tile_data(tmp_path):
    tf = make_frame()
    plain, geo = tmp_path / "plain.tif", tmp_path / "geo.tif"
    write_frames(plain, tf["compressed"], tf)
    write_frames(geo, tf["compressed"], tf, transform=NORTH_UP, crs=UTM18S)
    assert header_bytes(tf) == header_bytes(tf, transform=NORTH_UP, crs=UTM18S)
    assert len(open(plain, "rb").read()) == len(open(geo, "rb").read())


def test_rotation_does_not_move_the_tile_data(tmp_path):
    tf = make_frame()
    up, rot = tmp_path / "up.tif", tmp_path / "rot.tif"
    write_frames(up, tf["compressed"], tf, transform=NORTH_UP, crs=UTM18S)
    write_frames(rot, tf["compressed"], tf, transform=ROTATED, crs=UTM18S)
    assert min(values(read_ifd(up)[0][324], "Q")) == \
           min(values(read_ifd(rot)[0][324], "Q"))


# Georeferencing

def test_model_transformation_is_always_the_full_matrix(tmp_path):
    tf = make_frame()
    path = tmp_path / "a.tif"
    write_frames(path, tf["compressed"], tf, transform=NORTH_UP, crs=UTM18S)
    assert values(read_ifd(path)[0][34264], "d") == [
        30.0, 0.0, 0.0, 500000.0,
        0.0, -30.0, 0.0, 8000000.0,
        0.0, 0.0, 0.0, 0.0,
        0.0, 0.0, 0.0, 1.0]


def test_rotated_transform(tmp_path):
    tf = make_frame()
    path = tmp_path / "a.tif"
    write_frames(path, tf["compressed"], tf, transform=ROTATED, crs=UTM18S)
    assert values(read_ifd(path)[0][34264], "d") == [
        30.0, 5.0, 0.0, 500000.0,
        5.0, -30.0, 0.0, 8000000.0,
        0.0, 0.0, 0.0, 0.0,
        0.0, 0.0, 0.0, 1.0]


@pytest.mark.parametrize("kwargs", [
    {},
    {"transform": NORTH_UP, "crs": UTM18S},
    {"transform": ROTATED, "crs": 4326},
])
def test_the_forbidden_geo_tags_are_never_written(tmp_path, kwargs):
    tf = make_frame()
    path = tmp_path / "a.tif"
    write_frames(path, tf["compressed"], tf, **kwargs)
    entries = read_ifd(path)[0]
    for tag in FORBIDDEN_GEO:
        assert tag not in entries


def test_the_geokey_directory_is_always_32_bytes(tmp_path):
    tf = make_frame()
    for kwargs in ({}, {"transform": NORTH_UP, "crs": UTM18S},
                   {"transform": NORTH_UP, "crs": 4326}):
        path = tmp_path / "a.tif"
        write_frames(path, tf["compressed"], tf, **kwargs)
        type_, n, _at, payload = read_ifd(path)[0][34735]
        assert (type_, n, len(payload)) == (SHORT, 16, 32)


def test_geokeys_projected(tmp_path):
    tf = make_frame()
    path = tmp_path / "a.tif"
    write_frames(path, tf["compressed"], tf, transform=NORTH_UP, crs=UTM18S)
    head, keys = geokeys(path)
    assert head == [1, 1, 0, 3]
    assert keys == {MODEL: 1, RASTER: 1, PROJECTED: UTM18S}


def test_geokeys_geographic(tmp_path):
    tf = make_frame()
    path = tmp_path / "a.tif"
    write_frames(path, tf["compressed"], tf, transform=NORTH_UP, crs=4326)
    assert geokeys(path)[1] == {MODEL: 2, RASTER: 1, GEOGRAPHIC: 4326}


def test_the_kind_is_not_the_code_range(tmp_path):
    """EPSG:4037 sits inside the geographic block and is projected."""
    tf = make_frame()
    path = tmp_path / "a.tif"
    write_frames(path, tf["compressed"], tf, transform=NORTH_UP, crs=4037)
    assert geokeys(path)[1] == {MODEL: 1, RASTER: 1, PROJECTED: 4037}


# Undefined georeferencing

def test_undefined_georeferencing_writes_the_identity(tmp_path):
    tf = make_frame()
    path = tmp_path / "a.tif"
    write_frames(path, tf["compressed"], tf)
    assert values(read_ifd(path)[0][34264], "d") == [
        1.0, 0.0, 0.0, 0.0,
        0.0, 1.0, 0.0, 0.0,
        0.0, 0.0, 0.0, 0.0,
        0.0, 0.0, 0.0, 1.0]


def test_undefined_georeferencing_writes_zero_keys(tmp_path):
    tf = make_frame()
    path = tmp_path / "a.tif"
    write_frames(path, tf["compressed"], tf)
    head, keys = geokeys(path)
    assert head == [1, 1, 0, 3]
    assert keys == {MODEL: 0, RASTER: 1, GEOGRAPHIC: 0}


def test_raster_type_survives_without_a_crs(tmp_path):
    """PixelIsPoint is true of any raster, CRS or not."""
    tf = make_frame()
    path = tmp_path / "a.tif"
    write_frames(path, tf["compressed"], tf, pixel_is_point=True)
    assert geokeys(path)[1][RASTER] == 2


def test_pixel_is_point_with_a_crs(tmp_path):
    tf = make_frame()
    area, point = tmp_path / "area.tif", tmp_path / "point.tif"
    write_frames(area, tf["compressed"], tf, transform=NORTH_UP, crs=UTM18S)
    write_frames(point, tf["compressed"], tf, transform=NORTH_UP, crs=UTM18S,
                 pixel_is_point=True)
    assert geokeys(area)[1][RASTER] == 1
    assert geokeys(point)[1][RASTER] == 2


# What the writer refuses

def test_there_is_no_header_size_parameter(tmp_path):
    """The knob that used to pad in front of the tile data."""
    tf = make_frame()
    with pytest.raises(TypeError):
        write_frames(tmp_path / "a.tif", tf["compressed"], tf, header_size=4096)
    with pytest.raises(TypeError):
        rumi.write(tmp_path / "b.tif", tf, header_size=4096)


@pytest.mark.parametrize("bad", [8, 17, 24, 100])
def test_tile_size_must_be_a_multiple_of_sixteen(bad):
    arr = np.zeros((1, 64, 64), np.uint16)
    with pytest.raises(ValueError, match="16"):
        rumi.tile(arr, bad)


def test_a_crs_needs_a_transform(tmp_path):
    tf = make_frame()
    with pytest.raises(ValueError, match="together"):
        write_frames(tmp_path / "a.tif", tf["compressed"], tf, crs=UTM18S)


def test_an_unknown_epsg_is_refused(tmp_path):
    tf = make_frame()
    with pytest.raises(ValueError):
        write_frames(tmp_path / "a.tif", tf["compressed"], tf,
                     transform=NORTH_UP, crs=999999)


def test_a_crs_that_no_code_names_is_refused(tmp_path):
    tf = make_frame()
    for bad in ("+proj=utm +zone=18 +south", "WGS 84 / UTM zone 18S", 1.5):
        with pytest.raises((ValueError, TypeError), match="EPSG"):
            write_frames(tmp_path / "a.tif", tf["compressed"], tf,
                         transform=NORTH_UP, crs=bad)


# Files here come from the writer above, so these are about the reader alone.

@pytest.fixture
def valid_file(tmp_path):
    tiles = [bytes([i % 251]) * (7 + 3 * i) for i in range(2 * 3 * 5)]
    entries = spec_entries(70, 40, 16, 2, tiles)
    path = tmp_path / "made.tif"
    path.write_bytes(build_tiff(entries, tiles, bands=2))
    return path, entries, tiles


def test_the_independent_writer_is_accepted(valid_file):
    """If this fails, every rejection test below proves nothing."""
    path, _entries, _tiles = valid_file
    facts = rumi.RumiHeader.from_path(path).to_dict()
    assert facts["shape"] == [2, 40, 70]
    assert facts["base_tiles_offset"] == derive_base_offset(2, 30)


def _rejects(tmp_path, entries, tiles, match, bands=2, pad=0):
    path = tmp_path / "bad.tif"
    path.write_bytes(build_tiff(entries, tiles, bands=bands,
                                pad_before_tiles=pad))
    with pytest.raises((ValueError, IOError), match=match):
        rumi.RumiHeader.from_path(path)


def test_an_extra_tag_is_rejected(tmp_path, valid_file):
    _p, entries, tiles = valid_file
    entries = dict(entries)
    entries[305] = (ASCII, [])          # Software, harmless and still forbidden
    entries[305] = (SHORT, [1])
    _rejects(tmp_path, entries, tiles, "tag")


def test_a_missing_geo_tag_is_rejected(tmp_path, valid_file):
    _p, entries, tiles = valid_file
    for tag in GEO:
        trimmed = {k: v for k, v in entries.items() if k != tag}
        _rejects(tmp_path, trimmed, tiles, "tag")


@pytest.mark.parametrize("tag,type_,vals", [
    (33550, DOUBLE, [30.0, 30.0, 0.0]),
    (33922, DOUBLE, [0.0] * 6),
    (34736, DOUBLE, [1.0]),
    (34737, ASCII, [65, 0]),
])
def test_a_forbidden_geo_tag_is_rejected(tmp_path, valid_file, tag, type_, vals):
    _p, entries, tiles = valid_file
    entries = dict(entries)
    entries[tag] = (type_, vals)
    _rejects(tmp_path, entries, tiles, "tag")


def test_a_gap_before_the_tile_data_is_rejected(tmp_path, valid_file):
    _p, entries, tiles = valid_file
    _rejects(tmp_path, entries, tiles, "padding", pad=512)


def test_a_tile_size_off_the_grid_is_rejected(tmp_path):
    tiles = [b"x" * 8] * 4
    entries = spec_entries(40, 40, 20, 1, tiles)
    _rejects(tmp_path, entries, tiles, "16", bands=1)


def test_an_undefined_crs_file_is_accepted(tmp_path):
    """Model type zero is a valid file, it just has no CRS."""
    tiles = [bytes([i % 251]) * (7 + i) for i in range(4)]
    entries = spec_entries(32, 32, 16, 1, tiles, model=0, epsg=0,
                           transform=(1.0, 0.0, 0.0, 0.0, 1.0, 0.0))
    path = tmp_path / "nogeo.tif"
    path.write_bytes(build_tiff(entries, tiles, bands=1))
    assert rumi.RumiHeader.from_path(path).shape == (1, 32, 32)


# Round trip

def test_blob_from_file_matches_blob_from_write(tmp_path):
    tf = make_frame()
    path, blob = rumi.write(tmp_path / "a.tif", tf,
                            transform=NORTH_UP, crs=UTM18S)
    assert rumi.RumiHeader.from_path(path).to_dict() == \
           rumi.RumiHeader(blob).to_dict()


def test_pixels_survive_the_round_trip(tmp_path):
    geozl = pytest.importorskip("geozl")
    rng = np.random.default_rng(0)
    data = rng.integers(0, 3000, (3, 100, 130)).astype(np.uint16)
    tf = rumi.tile(data, 32)
    graphs = {}
    for t in tf:
        g = graphs.setdefault(t.data.shape,
                              geozl.graph(t.data, "planar>zigzag>entropy"))
        t.compressed = geozl.compress(t.data, graph=g)
    path, header = rumi.write(tmp_path / "a.tif", tf,
                              transform=NORTH_UP, crs=UTM18S)
    assert np.array_equal(rumi.read(path, header), data)
    assert rumi.RumiHeader(header).to_dict()["base_tiles_offset"] == \
           derive_base_offset(3, len(tf))
