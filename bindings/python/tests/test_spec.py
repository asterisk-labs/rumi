"""End-to-end conformance checks using an independent file builder."""

import struct

import numpy as np
import pytest
import rumi
from rumi import FrameTable
from rumi._write import header_bytes, write_frames

SHORT, LONG, LONG8, DOUBLE, ASCII = 3, 4, 16, 12, 2
TYPE_SIZE = {ASCII: 1, SHORT: 2, LONG: 4, DOUBLE: 8, LONG8: 8}

TAGS = (256, 257, 258, 277, 322, 323, 324, 325, 339, 34264, 34735,
        65000, 65001)
GEO = (34264, 34735)
FORBIDDEN_GEO = (33550, 33922, 34736, 34737)

IFD_OFFSET = 16
IFD_SIZE = 8 + 20 * len(TAGS) + 8          # 276
BASE_CONSTANT = IFD_OFFSET + IFD_SIZE      # 292

# FILE_MAGIC encodes ASCII "RUMI" as little-endian bytes.
FILE_MAGIC = 0x494D5552

MODEL, RASTER, GEOGRAPHIC, PROJECTED = 1024, 1025, 2048, 3072
TIME_MAGIC = 0x454D4954       # "TIME" on the wire
TRAILER_SIZE = 28
UTM18S = 32718
NORTH_UP = (30.0, 0.0, 500000.0, 0.0, -30.0, 8000000.0)
ROTATED = (30.0, 5.0, 500000.0, 5.0, -30.0, 8000000.0)


def derive_base_offset(bands, tiles, geo=True):
    """Compute the base offset directly from the specification."""
    external = ((2 * bands if bands >= 5 else 0)      # 258 BitsPerSample
                + (8 * tiles if tiles >= 2 else 0)    # 324 TileOffsets
                + (4 * tiles if tiles >= 3 else 0)    # 325 TileByteCounts
                + (2 * bands if bands >= 5 else 0)    # 339 SampleFormat
                + 128                                 # 34264
                + 32)                                 # 34735
    return BASE_CONSTANT + external


def make_frame(shape=(2, 40, 70), tile_size=16, dtype=np.uint16):
    """Return payloads with distinct lengths to expose ordering errors."""
    n = int(np.prod(shape[1:]))
    arr = np.arange(shape[0] * n, dtype=dtype).reshape(shape)
    tf = FrameTable.from_array(arr, "b (row h) (col w) -> row col b (h w)", tile_size)
    tf["compressed"] = [bytes([i % 251]) * (7 + 3 * i) for i in range(len(tf))]
    return tf


def read_ifd(path):
    """Return ordered IFD entries and file bytes from an independent parser."""
    blob = open(path, "rb").read() if not isinstance(path, bytes) else path
    magic, version, reserved = struct.unpack_from("<IHH", blob, 0)
    assert (magic, version, reserved) == (FILE_MAGIC, 1, 0)
    assert blob[:4] == b"RUMI"
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
    """Return the GeoKey header and an ID-to-value mapping."""
    v = values(read_ifd(path)[0][34735], "H")
    return v[:4], {v[4 + i * 4]: v[7 + i * 4] for i in range(v[3])}


def build_tiff(entries, tiles, bands=1, pad_before_tiles=0, trailer=None):
    """Build a file from tag values and frame-index-ordered payloads."""
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
        # Derive physical offsets in frame-index order.
        packed[324] = struct.pack("<" + "Q" * len(where), *where)

    out = bytearray(struct.pack("<IHHQ", FILE_MAGIC, 1, 0, IFD_OFFSET))
    out += struct.pack("<Q", len(ordered))
    for tag, (type_, vals) in ordered:
        raw = packed[tag]
        field = (struct.pack("<Q", external[tag]) if tag in external
                 else raw.ljust(8, b"\x00"))
        out += struct.pack("<HHQ", tag, type_, len(vals)) + field
    out += struct.pack("<Q", 0)
    for tag, (_type, _vals) in ordered:
        if tag in external:
            raw = packed[tag]
            out += raw + b"\x00" * (len(raw) & 1)
    out += b"\x00" * pad_before_tiles
    for payload in tiles:
        out += payload
    out += undefined_trailer() if trailer is None else trailer
    return bytes(out)


# Fixed trailer for undefined time.
def undefined_trailer():
    return struct.pack("<IHBBqqI", TIME_MAGIC, 1, 0, 0, 0, 0, 1)


def spec_entries(width, length, tile, bands, tiles, bits=16, fmt=1,
                 epsg=UTM18S, model=1, transform=NORTH_UP, unit=0, time=1):
    """Return the fixed tag set for a valid file."""
    t = transform
    matrix = [t[0], t[1], 0.0, t[2],
              t[3], t[4], 0.0, t[5],
              0.0, 0.0, 0.0, 0.0,
              0.0, 0.0, 0.0, 1.0]
    directory = [1, 1, 0, 3,
                 MODEL, 0, 1, model,
                 RASTER, 0, 1, 1,
                 (PROJECTED if model == 1 else GEOGRAPHIC), 0, 1, epsg]
    return {
        256: (LONG, [width]), 257: (LONG, [length]),
        258: (SHORT, [bits] * bands), 277: (SHORT, [bands]),
        322: (SHORT, [tile]), 323: (SHORT, [tile]),
        324: (LONG8, [0] * len(tiles)),
        325: (LONG, [len(t) for t in tiles]),
        339: (SHORT, [fmt] * bands),
        34264: (DOUBLE, matrix), 34735: (SHORT, directory),
        65000: (SHORT, [unit]),
        65001: (LONG, [time]),
    }


# Time trailer

def test_every_file_ends_with_a_time_trailer(tmp_path):
    """Return the fixed trailer for undefined time."""
    tf = make_frame()
    path = tmp_path / "a.rumi"
    write_frames(path, tf["compressed"], tf)
    blob = open(path, "rb").read()
    at = len(blob) - TRAILER_SIZE

    magic, ver, ttype, tbits, epoch, step, scale = struct.unpack_from(
        "<IHBBqqI", blob, at)
    assert blob[at:at + 4] == b"TIME"
    assert (magic, ver, ttype, tbits, epoch, step, scale) == \
        (TIME_MAGIC, 1, 0, 0, 0, 0, 1)
    assert at == header_bytes(tf) + sum(len(f) for f in tf["compressed"])


@pytest.mark.parametrize("field, value, because", [
    (0, b"TIMF", "no time trailer"),          # magic
    (4, struct.pack("<H", 2), "version"),     # version
    (6, b"\x03", "time_type"),                # time_type
    (24, struct.pack("<I", 0), "scale"),      # time_scale
    (24, struct.pack("<I", 7), "scale"),      # an unregistered scale
    (7, b"\x41", "time_bits"),                # time_bits, past what fits
])
def test_a_broken_trailer_is_refused(tmp_path, field, value, because):
    tf = make_frame()
    path = tmp_path / "a.rumi"
    write_frames(path, tf["compressed"], tf)
    blob = bytearray(open(path, "rb").read())
    at = len(blob) - TRAILER_SIZE + field
    blob[at:at + len(value)] = value
    bad = tmp_path / "bad.rumi"
    bad.write_bytes(blob)
    with pytest.raises(ValueError, match=because):
        rumi.RumiHeader.from_path(bad)


def test_a_file_cut_short_of_its_trailer_is_refused(tmp_path):
    tf = make_frame()
    path = tmp_path / "a.rumi"
    write_frames(path, tf["compressed"], tf)
    blob = open(path, "rb").read()
    short = tmp_path / "short.rumi"
    short.write_bytes(blob[:-4])
    with pytest.raises(ValueError, match="no room for"):
        rumi.RumiHeader.from_path(short)


# Fixed IFD

def test_the_tag_set_is_exactly_the_thirteen(tmp_path):
    tf = make_frame()
    path = tmp_path / "a.rumi"
    write_frames(path, tf["compressed"], tf, transform=NORTH_UP, crs=UTM18S)
    assert tuple(sorted(read_ifd(path)[0])) == TAGS


def test_the_tag_set_does_not_depend_on_georeferencing(tmp_path):

    tf = make_frame()
    plain, geo = tmp_path / "plain.rumi", tmp_path / "geo.rumi"
    write_frames(plain, tf["compressed"], tf)
    write_frames(geo, tf["compressed"], tf, transform=NORTH_UP, crs=UTM18S)
    assert tuple(sorted(read_ifd(plain)[0])) == TAGS
    assert set(read_ifd(plain)[0]) == set(read_ifd(geo)[0])


def test_tags_are_in_ascending_order(tmp_path):
    tf = make_frame()
    path = tmp_path / "a.rumi"
    write_frames(path, tf["compressed"], tf)
    tags = list(read_ifd(path)[0])
    assert tags == sorted(tags)


def test_the_ifd_starts_at_sixteen_and_is_one(tmp_path):
    tf = make_frame()
    path = tmp_path / "a.rumi"
    write_frames(path, tf["compressed"], tf)
    _entries, next_ifd, blob = read_ifd(path)
    assert struct.unpack_from("<Q", blob, 8) == (IFD_OFFSET,)
    assert next_ifd == 0


def test_the_ifd_is_always_276_bytes(tmp_path):
    tf = make_frame()
    path = tmp_path / "a.rumi"
    write_frames(path, tf["compressed"], tf)
    (count,) = struct.unpack_from("<Q", read_ifd(path)[2], IFD_OFFSET)
    assert count == len(TAGS)
    assert 8 + 20 * count + 8 == IFD_SIZE


# Placement

def test_values_are_inline_when_they_fit(tmp_path):
    tf = make_frame()
    path = tmp_path / "a.rumi"
    write_frames(path, tf["compressed"], tf)
    for tag, (type_, n, at, _payload) in read_ifd(path)[0].items():
        assert (at is None) == (TYPE_SIZE[type_] * n <= 8), tag


def test_external_values_are_packed_right_after_the_ifd(tmp_path):
    tf = make_frame()
    path = tmp_path / "a.rumi"
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


def test_no_gap_before_frame_data(tmp_path):
    tf = make_frame()
    path = tmp_path / "a.rumi"
    write_frames(path, tf["compressed"], tf)
    blob = open(path, "rb").read()
    base = header_bytes(tf)
    payloads = b"".join(tf["compressed"])
    assert blob[base:base + len(payloads)] == payloads
    assert len(blob) == base + len(payloads) + TRAILER_SIZE


# Deriving base_frame_offset

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
    path = tmp_path / "a.rumi"
    write_frames(path, tf["compressed"], tf, transform=NORTH_UP, crs=UTM18S)
    want = derive_base_offset(tf.bands, len(tf))
    assert header_bytes(tf) == want
    assert min(values(read_ifd(path)[0][324], "Q")) == want


def test_the_blob_agrees_with_the_derivation(tmp_path):
    tf = make_frame()
    _path, blob = rumi.write(tmp_path / "a.rumi", tf)
    facts = rumi.RumiHeader(blob).to_dict()
    assert facts["base_frame_offset"] == derive_base_offset(tf.bands, len(tf))


def test_the_same_shape_gives_the_same_offset(tmp_path):
    """Different payload sizes, same first tile."""
    a = make_frame()
    b = make_frame()
    b["compressed"] = [bytes(1 + i) for i in range(len(b))]
    pa = tmp_path / "a.rumi"
    pb = tmp_path / "b.rumi"
    write_frames(pa, a["compressed"], a)
    write_frames(pb, b["compressed"], b)
    assert min(values(read_ifd(pa)[0][324], "Q")) == \
           min(values(read_ifd(pb)[0][324], "Q"))


def test_georeferencing_does_not_move_the_tile_data(tmp_path):
    tf = make_frame()
    plain, geo = tmp_path / "plain.rumi", tmp_path / "geo.rumi"
    write_frames(plain, tf["compressed"], tf)
    write_frames(geo, tf["compressed"], tf, transform=NORTH_UP, crs=UTM18S)
    assert header_bytes(tf) == header_bytes(tf, transform=NORTH_UP, crs=UTM18S)
    assert len(open(plain, "rb").read()) == len(open(geo, "rb").read())


def test_rotation_does_not_move_the_tile_data(tmp_path):
    tf = make_frame()
    up, rot = tmp_path / "up.rumi", tmp_path / "rot.rumi"
    write_frames(up, tf["compressed"], tf, transform=NORTH_UP, crs=UTM18S)
    write_frames(rot, tf["compressed"], tf, transform=ROTATED, crs=UTM18S)
    assert min(values(read_ifd(up)[0][324], "Q")) == \
           min(values(read_ifd(rot)[0][324], "Q"))


# Georeferencing

def test_model_transformation_is_always_the_full_matrix(tmp_path):
    tf = make_frame()
    path = tmp_path / "a.rumi"
    write_frames(path, tf["compressed"], tf, transform=NORTH_UP, crs=UTM18S)
    assert values(read_ifd(path)[0][34264], "d") == [
        30.0, 0.0, 0.0, 500000.0,
        0.0, -30.0, 0.0, 8000000.0,
        0.0, 0.0, 0.0, 0.0,
        0.0, 0.0, 0.0, 1.0]


def test_rotated_transform(tmp_path):
    tf = make_frame()
    path = tmp_path / "a.rumi"
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
    path = tmp_path / "a.rumi"
    write_frames(path, tf["compressed"], tf, **kwargs)
    entries = read_ifd(path)[0]
    for tag in FORBIDDEN_GEO:
        assert tag not in entries


def test_the_geokey_directory_is_always_32_bytes(tmp_path):
    tf = make_frame()
    for kwargs in ({}, {"transform": NORTH_UP, "crs": UTM18S},
                   {"transform": NORTH_UP, "crs": 4326}):
        path = tmp_path / "a.rumi"
        write_frames(path, tf["compressed"], tf, **kwargs)
        type_, n, _at, payload = read_ifd(path)[0][34735]
        assert (type_, n, len(payload)) == (SHORT, 16, 32)


def test_geokeys_projected(tmp_path):
    tf = make_frame()
    path = tmp_path / "a.rumi"
    write_frames(path, tf["compressed"], tf, transform=NORTH_UP, crs=UTM18S)
    head, keys = geokeys(path)
    assert head == [1, 1, 0, 3]
    assert keys == {MODEL: 1, RASTER: 1, PROJECTED: UTM18S}


def test_geokeys_geographic(tmp_path):
    tf = make_frame()
    path = tmp_path / "a.rumi"
    write_frames(path, tf["compressed"], tf, transform=NORTH_UP, crs=4326)
    assert geokeys(path)[1] == {MODEL: 2, RASTER: 1, GEOGRAPHIC: 4326}


def test_the_kind_is_not_the_code_range(tmp_path):
    """EPSG:4037 is projected despite its numeric neighborhood."""
    tf = make_frame()
    path = tmp_path / "a.rumi"
    write_frames(path, tf["compressed"], tf, transform=NORTH_UP, crs=4037)
    assert geokeys(path)[1] == {MODEL: 1, RASTER: 1, PROJECTED: 4037}


# Undefined georeferencing

def test_undefined_georeferencing_writes_the_identity(tmp_path):
    tf = make_frame()
    path = tmp_path / "a.rumi"
    write_frames(path, tf["compressed"], tf)
    assert values(read_ifd(path)[0][34264], "d") == [
        1.0, 0.0, 0.0, 0.0,
        0.0, 1.0, 0.0, 0.0,
        0.0, 0.0, 0.0, 0.0,
        0.0, 0.0, 0.0, 1.0]


def test_undefined_georeferencing_writes_zero_keys(tmp_path):
    tf = make_frame()
    path = tmp_path / "a.rumi"
    write_frames(path, tf["compressed"], tf)
    head, keys = geokeys(path)
    assert head == [1, 1, 0, 3]
    assert keys == {MODEL: 0, RASTER: 1, GEOGRAPHIC: 0}


def test_raster_type_survives_without_a_crs(tmp_path):
    """PixelIsPoint is true of any raster, CRS or not."""
    tf = make_frame()
    path = tmp_path / "a.rumi"
    write_frames(path, tf["compressed"], tf, pixel_is_point=True)
    assert geokeys(path)[1][RASTER] == 2


def test_pixel_is_point_with_a_crs(tmp_path):
    tf = make_frame()
    area, point = tmp_path / "area.rumi", tmp_path / "point.rumi"
    write_frames(area, tf["compressed"], tf, transform=NORTH_UP, crs=UTM18S)
    write_frames(point, tf["compressed"], tf, transform=NORTH_UP, crs=UTM18S,
                 pixel_is_point=True)
    assert geokeys(area)[1][RASTER] == 1
    assert geokeys(point)[1][RASTER] == 2


# Writer validation

def test_there_is_no_header_size_parameter(tmp_path):
    tf = make_frame()
    with pytest.raises(TypeError):
        write_frames(tmp_path / "a.rumi", tf["compressed"], tf, header_size=4096)
    with pytest.raises(TypeError):
        rumi.write(tmp_path / "b.rumi", tf, header_size=4096)


@pytest.mark.parametrize("size", [1, 8, 17, 24, 100, 65535])
def test_tile_size_accepts_positive_uint16_values(size):
    arr = np.zeros((1, 64, 64), np.uint16)
    assert rumi.frames(arr, "b (row h) (col w) -> row col b (h w)", size).tile_size == size


@pytest.mark.parametrize("bad", [0, -1, 65536])
def test_tile_size_must_fit_its_uint16_field(bad):
    arr = np.zeros((1, 64, 64), np.uint16)
    with pytest.raises(ValueError, match="65535"):
        rumi.frames(arr, "b (row h) (col w) -> row col b (h w)", bad)


def test_a_crs_needs_a_transform(tmp_path):
    tf = make_frame()
    with pytest.raises(ValueError, match="together"):
        write_frames(tmp_path / "a.rumi", tf["compressed"], tf, crs=UTM18S)


def test_an_unknown_epsg_is_refused(tmp_path):
    tf = make_frame()
    with pytest.raises(ValueError):
        write_frames(tmp_path / "a.rumi", tf["compressed"], tf,
                     transform=NORTH_UP, crs=999999)


def test_a_crs_that_no_code_names_is_refused(tmp_path):
    tf = make_frame()
    for bad in ("+proj=utm +zone=18 +south", "WGS 84 / UTM zone 18S", 1.5):
        with pytest.raises((ValueError, TypeError), match="EPSG"):
            write_frames(tmp_path / "a.rumi", tf["compressed"], tf,
                         transform=NORTH_UP, crs=bad)


# Reader validation uses files from the independent builder above.

@pytest.fixture
def valid_file(tmp_path):
    tiles = [bytes([i % 251]) * (7 + 3 * i) for i in range(2 * 3 * 5)]
    entries = spec_entries(70, 40, 16, 2, tiles)
    path = tmp_path / "made.rumi"
    path.write_bytes(build_tiff(entries, tiles, bands=2))
    return path, entries, tiles


def test_the_independent_writer_is_accepted(valid_file):
    """Confirm that the independent builder produces a valid baseline file."""
    path, _entries, _tiles = valid_file
    facts = rumi.RumiHeader.from_path(path).to_dict()
    assert facts["shape"] == [2, 40, 70]
    assert facts["base_frame_offset"] == derive_base_offset(2, 30)


def _rejects(tmp_path, entries, tiles, match, bands=2, pad=0):
    path = tmp_path / "bad.rumi"
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


def test_tags_with_the_wrong_tiff_type_are_rejected(tmp_path, valid_file):
    _path, entries, tiles = valid_file
    entries = dict(entries)
    entries[256] = (SHORT, [70])
    _rejects(tmp_path, entries, tiles, "type")


def test_tags_out_of_rising_order_are_rejected(tmp_path, valid_file):
    _path, entries, tiles = valid_file
    blob = bytearray(build_tiff(entries, tiles, bands=2))
    first = IFD_OFFSET + 8
    a = bytes(blob[first:first + 20])
    blob[first:first + 20] = blob[first + 20:first + 40]
    blob[first + 20:first + 40] = a
    path = tmp_path / "bad-order.rumi"
    path.write_bytes(blob)
    with pytest.raises(ValueError, match="order"):
        rumi.RumiHeader.from_path(path)


def test_external_values_out_of_place_are_rejected(tmp_path, valid_file):
    _path, entries, tiles = valid_file
    blob = bytearray(build_tiff(entries, tiles, bands=2))
    transform_entry = IFD_OFFSET + 8 + TAGS.index(34264) * 20
    (at,) = struct.unpack_from("<Q", blob, transform_entry + 12)
    struct.pack_into("<Q", blob, transform_entry + 12, at + 8)
    path = tmp_path / "bad-placement.rumi"
    path.write_bytes(blob)
    with pytest.raises(ValueError, match="expected"):
        rumi.RumiHeader.from_path(path)


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


def test_an_unaligned_tile_size_is_accepted(tmp_path):
    tiles = [b"x" * 8] * 4
    entries = spec_entries(40, 40, 20, 1, tiles)
    path = tmp_path / "unaligned.rumi"
    path.write_bytes(build_tiff(entries, tiles, bands=1))
    assert rumi.RumiHeader.from_path(path).to_dict()["tile"] == [20, 20]


def test_an_overflowing_tile_frame_count_is_rejected(tmp_path):
    entries = spec_entries(0xFFFFFFFF, 0xFFFFFFFF, 16, 256, [])
    _rejects(tmp_path, entries, [], "overflows", bands=256)


def test_an_undefined_crs_file_is_accepted(tmp_path):
    """GTModelTypeGeoKey zero represents a valid undefined CRS."""
    tiles = [bytes([i % 251]) * (7 + i) for i in range(4)]
    entries = spec_entries(32, 32, 16, 1, tiles, model=0, epsg=0,
                           transform=(1.0, 0.0, 0.0, 0.0, 1.0, 0.0))
    path = tmp_path / "nogeo.rumi"
    path.write_bytes(build_tiff(entries, tiles, bands=1))
    assert rumi.RumiHeader.from_path(path).shape == (1, 32, 32)


# Round trip

def test_blob_from_file_matches_blob_from_write(tmp_path):
    tf = make_frame()
    path, blob = rumi.write(tmp_path / "a.rumi", tf,
                            transform=NORTH_UP, crs=UTM18S)
    assert rumi.RumiHeader.from_path(path).to_dict() == \
           rumi.RumiHeader(blob).to_dict()


def test_pixels_survive_the_round_trip(tmp_path):
    geozl = pytest.importorskip("geozl")
    rng = np.random.default_rng(0)
    data = rng.integers(0, 3000, (3, 100, 130)).astype(np.uint16)
    tf = rumi.frames(data, "b (row h) (col w) -> row col b (h w)", 32)
    graphs = {}
    for t in tf:
        g = graphs.setdefault(t.data.shape,
                              geozl.graph(t.data, "planar>zigzag>entropy"))
        t.compressed = geozl.compress(t.data, graph=g)
    path, header = rumi.write(tmp_path / "a.rumi", tf,
                              transform=NORTH_UP, crs=UTM18S)
    assert np.array_equal(rumi.read(path, header), data)
    assert rumi.RumiHeader(header).to_dict()["base_frame_offset"] == \
           derive_base_offset(3, len(tf))


# Regression cases for validation failures.

def round_half_up(a, b):
    """The slope the specification mandates, in Python's floor division."""
    return (2 * a + b) // (2 * b)


def pack_trailer(kind, coords, scale=86400, epoch=None, step=None, bits=None,
                 zigzagged=None, spare=0):
    """Build a canonical trailer while allowing one field to be overridden."""
    c = list(coords)
    e = c[0] if epoch is None else epoch
    if step is None:
        step = 0 if len(c) < 2 else round_half_up(c[-1] - c[0], len(c) - 1)
    zz = zigzagged
    if zz is None:
        res = [c[i] - (e + i * step) for i in range(len(c))]
        zz = [2 * x if x >= 0 else -2 * x - 1 for x in res]
    width = (max(zz).bit_length() if bits is None else bits)
    packed = bytearray((len(c) * width + 7) // 8)
    for i, v in enumerate(zz):
        for j in range(width):
            if v >> j & 1:
                packed[(i * width + j) // 8] |= 1 << ((i * width + j) % 8)
    if spare and packed:
        packed[-1] |= spare
    return struct.pack("<IHBBqqI", TIME_MAGIC, 1, kind, width, e, step,
                       scale) + bytes(packed)


def dated_file(path, trailer, steps, **kw):
    """Build a one-cell file with the supplied trailer."""
    # Unit 1 stores the time axis inside a single cell frame.
    tiles = [b"\x01\x02\x03\x04"]
    entries = spec_entries(16, 16, 16, 1, tiles, unit=1, time=steps, **kw)
    path.write_bytes(build_tiff(entries, tiles, trailer=trailer))
    return path


def test_a_sample_format_wider_than_a_byte_is_refused(tmp_path):
    """Validate SampleFormat before narrowing it to the header's uint8 field."""
    tiles = [b"\x01\x02\x03\x04"]
    entries = spec_entries(16, 16, 16, 1, tiles, unit=0, time=1)
    entries[339] = (SHORT, [257])
    path = tmp_path / "sf.rumi"
    path.write_bytes(build_tiff(entries, tiles))
    with pytest.raises(ValueError, match="sample_format=257"):
        rumi.RumiHeader.from_path(path)


def test_an_undefined_crs_needs_the_geographic_key(tmp_path):
    """Undefined CRS uses GeographicTypeGeoKey 2048 with value zero."""
    tiles = [b"\x01\x02\x03\x04"]
    # Start from valid undefined georeferencing and alter only the key ID.
    entries = spec_entries(16, 16, 16, 1, tiles, unit=0, time=1, model=0,
                           epsg=0, transform=(1.0, 0.0, 0.0, 0.0, 1.0, 0.0))
    directory = list(entries[34735][1])
    directory[12] = PROJECTED
    entries[34735] = (SHORT, directory)
    path = tmp_path / "gk.rumi"
    path.write_bytes(build_tiff(entries, tiles))
    with pytest.raises(ValueError, match="key 2048"):
        rumi.RumiHeader.from_path(path)


def test_more_coordinates_than_the_reader_will_hold_are_refused(tmp_path):
    """Resource limits bound a zero-width residual axis before allocation."""
    huge = 400_000_000
    trailer = struct.pack("<IHBBqqI", TIME_MAGIC, 1, 2, 0, 0, 1, 86400)
    path = dated_file(tmp_path / "big.rumi", trailer, huge)
    with pytest.raises(ValueError, match="this reader will allocate"):
        rumi.RumiHeader.from_path(path)


@pytest.mark.parametrize("because, kw", [
    ("time_epoch",  {"epoch": 19000}),                  # not the first coordinate
    ("time_step",   {"step": 40}),                      # not the slope implied
    ("time_bits",   {"bits": 12}),                      # wider than needed
])
def test_a_trailer_that_is_not_canonical_is_refused(tmp_path, because, kw):
    """Reject non-canonical encodings of otherwise valid coordinates."""
    coords = [19723, 19754, 19782, 19813]
    path = dated_file(tmp_path / "canon.rumi",
                      pack_trailer(2, coords, **kw), len(coords))
    with pytest.raises(ValueError, match=because):
        rumi.RumiHeader.from_path(path)


@pytest.mark.parametrize("kind, coords, because", [
    (2, [19723, 19700, 19800], "goes backwards"),        # instants go back
    (1, [19723, 19723, 19800, 19850], "covers nothing"),  # a step with no width
    (1, [19723, 19800, 19750, 19820], "steps that overlap"),
])
def test_steps_out_of_order_are_refused(tmp_path, kind, coords, because):
    steps = len(coords) if kind == 2 else len(coords) // 2
    path = dated_file(tmp_path / "order.rumi",
                      pack_trailer(kind, coords), steps)
    with pytest.raises(ValueError, match=because):
        rumi.RumiHeader.from_path(path)


def test_a_canonical_trailer_built_from_the_spec_is_accepted(tmp_path):
    """Accept the canonical baseline used by the preceding rejection cases."""
    coords = [19723, 19754, 19782, 19813]
    path = dated_file(tmp_path / "ok.rumi", pack_trailer(2, coords), len(coords))
    assert rumi.RumiHeader.from_path(path).time_count == 4
    assert rumi.read_time(path).steps == [
        __import__("datetime").date(1970, 1, 1)
        + __import__("datetime").timedelta(days=d) for d in coords]


def test_padding_left_in_the_last_residual_byte_is_refused(tmp_path):
    """Unused bits in the final residual byte must be zero."""
    coords = [19723, 19754, 19782]
    good = pack_trailer(2, coords)
    assert len(good) == TRAILER_SIZE + 1
    path = dated_file(tmp_path / "pad.rumi",
                      pack_trailer(2, coords, spare=0xC0), len(coords))
    with pytest.raises(ValueError, match="unused bits"):
        rumi.RumiHeader.from_path(path)
    ok = dated_file(tmp_path / "nopad.rumi", good, len(coords))
    assert rumi.RumiHeader.from_path(ok).time_count == 3


def blob(width, length, tile, bands=1, steps=1, unit=0, count_min=1, bits=16):
    """Build an external header directly from field values."""
    return struct.pack("<IHIIIHHHBBBIB", 0x45564F4C, 1, width, length, steps,
                       tile, tile, bands, bits, 1, unit, count_min, 0)


def test_a_window_wider_than_the_reader_will_hold_is_refused():
    """Reject range plans that exceed the configured resource limit."""
    from rumi._ffi import _Spec, ffi, lib
    spec = _Spec(blob(60000, 60000, 1))
    out = ffi.new("rumi_range**")
    count = ffi.new("size_t*")
    rc = lib.rumi_plan_ranges(spec.handle, ffi.NULL, 0, ffi.NULL, 0,
                              0, 60000, 0, 60000, out, count)
    assert rc == lib.RUMI_ERR_INVALID
    assert "this reader will allocate" in ffi.string(lib.rumi_last_error()).decode()


def test_a_window_the_reader_can_hold_is_still_planned():
    """Resource limits do not reject plans that fit within the limit."""
    from rumi._ffi import _Spec, ffi, lib
    spec = _Spec(blob(64, 64, 16))
    out = ffi.new("rumi_range**")
    count = ffi.new("size_t*")
    assert lib.rumi_plan_ranges(spec.handle, ffi.NULL, 0, ffi.NULL, 0,
                                0, 64, 0, 64, out, count) == lib.RUMI_OK
    assert count[0] == 16          # 4 x 4 tiles, one band, one step
    lib.rumi_free(out[0])


def test_many_bands_do_not_make_planning_quadratic():
    """Range planning scales linearly with the number of returned frames."""
    import time

    from rumi._ffi import _Spec, ffi, lib
    spec = _Spec(blob(16, 272, 32, bands=63745))
    out = ffi.new("rumi_range**")
    count = ffi.new("size_t*")
    start = time.perf_counter()
    rc = lib.rumi_plan_ranges(spec.handle, ffi.NULL, 0, ffi.NULL, 0,
                              0, 272, 0, 16, out, count)
    elapsed = time.perf_counter() - start
    assert rc == lib.RUMI_OK
    assert count[0] == 9 * 63745       # 9 grid positions, one frame per band
    lib.rumi_free(out[0])
    # Leave ample margin for slower CI hosts.
    assert elapsed < 2.0, f"planning took {elapsed:.1f}s"


# The rumi file header is distinct from TIFF and BigTIFF signatures.

def test_the_file_header_is_rumis_own(tmp_path):
    tf = make_frame()
    path = tmp_path / "a.rumi"
    write_frames(path, tf["compressed"], tf)
    head = open(path, "rb").read(16)

    assert head[:4] == b"RUMI" == bytes.fromhex("52554d49")
    magic, version, reserved = struct.unpack_from("<IHH", head, 0)
    assert (magic, version, reserved) == (FILE_MAGIC, 1, 0)
    assert struct.unpack_from("<Q", head, 8)[0] == IFD_OFFSET
    # It contains neither a TIFF byte-order mark nor a TIFF version.
    assert head[:2] not in (b"II", b"MM")
    assert struct.unpack_from("<H", head, 2)[0] not in (42, 43)


@pytest.mark.parametrize("at, value, because", [
    (0, b"II+\x00", "not a rumi file"),        # BigTIFF signature
    (0, b"RUMJ", "not a rumi file"),           # invalid rumi magic
    (4, struct.pack("<H", 2), "file version"),
    (6, struct.pack("<H", 1), "reserved"),
    (8, struct.pack("<Q", 24), "IFD at byte"),
])
def test_a_foreign_file_header_is_refused(tmp_path, at, value, because):
    tf = make_frame()
    path = tmp_path / "a.rumi"
    write_frames(path, tf["compressed"], tf)
    blob = bytearray(open(path, "rb").read())
    blob[at:at + len(value)] = value
    bad = tmp_path / "bad.rumi"
    bad.write_bytes(blob)
    with pytest.raises(ValueError, match=because):
        rumi.RumiHeader.from_path(bad)


def test_an_inline_value_leaves_no_room_for_a_second_spelling(tmp_path):
    """Unused bytes in an inline IFD value must be zero."""
    tf = make_frame()
    path = tmp_path / "a.rumi"
    write_frames(path, tf["compressed"], tf)
    blob = open(path, "rb").read()
    (first,) = struct.unpack_from("<Q", blob, 8)
    (count,) = struct.unpack_from("<Q", blob, first)
    at_256 = None
    for i in range(count):
        pos = first + 8 + 20 * i
        tag, type_, n = struct.unpack_from("<HHQ", blob, pos)
        size = TYPE_SIZE[type_] * n
        if size <= 8:
            assert blob[pos + 12 + size:pos + 20] == b"\x00" * (8 - size), tag
        if tag == 256:
            at_256 = pos + 12 + size       # tag 256 is a LONG, 4 of 8 bytes
    assert at_256 is not None

    dirty = bytearray(blob)
    dirty[at_256] = 0xAB
    bad = tmp_path / "bad.rumi"
    bad.write_bytes(dirty)
    with pytest.raises(ValueError, match="inline bytes"):
        rumi.RumiHeader.from_path(bad)


def test_the_blob_a_writer_returns_is_the_one_the_file_yields(tmp_path):
    """The writer returns the header rebuilt from the finalized file."""
    for kw in ({}, {"transform": NORTH_UP, "crs": UTM18S},
               {"time": ["2024-08-25"]}):
        tf = make_frame()
        path = tmp_path / "a.rumi"
        written = write_frames(path, tf["compressed"], tf, **kw)
        assert written == rumi._ffi._header_from_file(path)


# Derived frame sizes must reject uint64 multiplication overflow.

def test_a_frame_size_that_wraps_uint64_is_refused():
    """Reject a decoded frame size whose factors multiply to 2**64."""
    from rumi._ffi import _Spec
    raw = struct.pack("<IHIIIHHHBBBIB", 0x45564F4C, 1, 65535, 65535, 32768,
                      32768, 32768, 32768, 128, 6, 3, 1, 0)
    assert 32768 * 32768 * 16 * 32768 * 32768 == 2 ** 64
    with pytest.raises(ValueError, match="overflow"):
        _Spec(raw)


def test_more_steps_than_a_read_can_name_are_refused():
    """Require an explicit selection when time_count exceeds int indexing."""
    from rumi._ffi import _Spec, ffi, lib
    # Keep frame allocation small while time_count exceeds the int index range.
    spec = _Spec(blob(1, 1, 1, steps=1 << 30, unit=1, bits=8))
    out = ffi.new("rumi_range**")
    count = ffi.new("size_t*")
    rc = lib.rumi_plan_ranges(spec.handle, ffi.NULL, 0, ffi.NULL, 0,
                              0, 1, 0, 1, out, count)
    assert rc == lib.RUMI_ERR_INVALID
    assert "pass the steps you want" in ffi.string(lib.rumi_last_error()).decode()
    # Explicitly selecting a representable step remains valid.
    one = ffi.new("int[]", [1])
    assert lib.rumi_plan_ranges(spec.handle, one, 1, ffi.NULL, 0,
                                0, 1, 0, 1, out, count) == lib.RUMI_OK
    lib.rumi_free(out[0])


@pytest.mark.parametrize("because, mutate", [
    ("element 8", lambda e: e.__setitem__(
        34264, (DOUBLE, [*e[34264][1][:8], 7.0, *e[34264][1][9:]]))),
    ("element 2", lambda e: e.__setitem__(
        34264, (DOUBLE, [*e[34264][1][:2], 7.0, *e[34264][1][3:]]))),
    ("element 15", lambda e: e.__setitem__(
        34264, (DOUBLE, [*e[34264][1][:15], 2.0]))),
    ("key_id, 0, 1, value", lambda e: e.__setitem__(
        34735, (SHORT, [*e[34735][1][:5], 9, *e[34735][1][6:]]))),
    ("key_id, 0, 1, value", lambda e: e.__setitem__(
        34735, (SHORT, [*e[34735][1][:6], 4, *e[34735][1][7:]]))),
])
def test_georeferencing_fields_have_one_legal_shape(tmp_path, because, mutate):
    """Reject non-canonical fixed entries in either georeferencing tag."""
    tiles = [b"\x01\x02\x03\x04"]
    entries = spec_entries(16, 16, 16, 1, tiles, unit=0, time=1)
    mutate(entries)
    path = tmp_path / "geo.rumi"
    path.write_bytes(build_tiff(entries, tiles))
    with pytest.raises(ValueError, match=because):
        rumi.RumiHeader.from_path(path)


def test_a_decoded_sub_byte_frame_proves_its_padding(tmp_path):
    """Readers validate unused high bits after decoding sub-byte samples."""
    pytest.importorskip("ml_dtypes")
    geozl = pytest.importorskip("geozl")
    for fill, ok in ((0x07, True), (0xF1, False)):
        buf = np.full((16, 16), fill, np.uint8)
        payload = geozl.compress(buf, graph=geozl.graph(buf, "planar>zigzag>zstd"))
        entries = spec_entries(16, 16, 16, 1, [payload], bits=4, fmt=1,
                               unit=0, time=1)
        path = tmp_path / f"{fill:02x}.rumi"
        path.write_bytes(build_tiff(entries, [payload]))
        header = rumi._ffi._header_from_file(path)
        if ok:
            assert np.unique(np.asarray(rumi.read(path, header))) == [fill]
        else:
            with pytest.raises(Exception, match="bits set above"):
                rumi.read(path, header)


def test_an_undefined_crs_carries_the_matrix_the_spec_fixes(tmp_path):
    """Undefined CRS requires the fixed transformation matrix."""
    tiles = [b"\x01\x02\x03\x04"]
    entries = spec_entries(16, 16, 16, 1, tiles, unit=0, time=1, model=0,
                           epsg=0, transform=(1.0, 0.0, 0.0, 0.0, 1.0, 0.0))
    path = tmp_path / "none.rumi"
    path.write_bytes(build_tiff(entries, tiles))
    assert rumi.RumiHeader.from_path(path).frames == 1

    entries[34264] = (DOUBLE, [30.0, *entries[34264][1][1:]])
    bad = tmp_path / "none-but-placed.rumi"
    bad.write_bytes(build_tiff(entries, tiles))
    with pytest.raises(ValueError, match="undefined CRS carries"):
        rumi.RumiHeader.from_path(bad)


def test_a_sub_byte_window_reads_like_any_other(tmp_path):
    """Sub-byte windows use the same byte-stride arithmetic as uint8."""
    pytest.importorskip("ml_dtypes")
    geozl = pytest.importorskip("geozl")
    src = (np.arange(32 * 32, dtype=np.uint8) & 0x0F).reshape(32, 32)
    payloads = [geozl.compress(t, graph=geozl.graph(t, "planar>zigzag>zstd"))
                for t in (np.ascontiguousarray(src[r * 16:(r + 1) * 16,
                                                   c * 16:(c + 1) * 16])
                          for r in range(2) for c in range(2))]
    entries = spec_entries(32, 32, 16, 1, payloads, bits=4, fmt=1, unit=0, time=1)
    path = tmp_path / "sb.rumi"
    path.write_bytes(build_tiff(entries, payloads))
    header = rumi._ffi._header_from_file(path)

    assert np.array_equal(np.asarray(rumi.read(path, header))[0], src)
    for y, x in (((0, 16), (16, 32)), ((8, 24), (4, 28)), ((13, 31), (2, 30))):
        got = np.asarray(rumi.read(path, header, y=y, x=x))
        assert np.array_equal(got[0], src[y[0]:y[1], x[0]:x[1]]), (y, x)


def test_a_file_hands_back_the_georeferencing_it_was_given(tmp_path):
    """Read georeferencing from the IFD after a writer round trip."""
    for kw, want in (
        ({"transform": NORTH_UP, "crs": UTM18S},
         (NORTH_UP, UTM18S, False)),
        ({"transform": ROTATED, "crs": UTM18S, "pixel_is_point": True},
         (ROTATED, UTM18S, True)),
        ({}, (None, None, False)),
    ):
        tf = make_frame()
        path = tmp_path / "geo.rumi"
        write_frames(path, tf["compressed"], tf, **kw)
        got = rumi.read_geo(path)
        assert got.crs == want[1]
        assert got.pixel_is_point == want[2]
        assert got.transform == want[0]


def test_a_sub_byte_stack_reads_without_dlpack(tmp_path):
    """Sub-byte stacks use the materialized numpy read path."""
    pytest.importorskip("ml_dtypes")
    geozl = pytest.importorskip("geozl")
    paths, headers, want = [], [], []
    for i in range(3):
        buf = np.full((16, 16), 0x03 + i, np.uint8)
        payload = geozl.compress(buf, graph=geozl.graph(buf, "planar>zigzag>zstd"))
        entries = spec_entries(16, 16, 16, 1, [payload], bits=4, fmt=1,
                               unit=0, time=1)
        path = tmp_path / f"sb{i}.rumi"
        path.write_bytes(build_tiff(entries, [payload]))
        paths.append(path)
        headers.append(rumi._ffi._header_from_file(path))
        want.append(buf)

    got = np.asarray(rumi.read(paths, headers))
    assert np.array_equal(got[:, 0], np.stack(want))
    picked = np.asarray(rumi.read(paths, headers, n=[2, 0]))
    assert np.array_equal(picked[:, 0], np.stack([want[2], want[0]]))
