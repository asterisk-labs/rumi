"""Validate writer output with an independent file parser."""

import hashlib
import struct

import numpy as np
import pytest
import rumi
from _labels import labels
from rumi import FrameTable
from rumi._ffi import _Spec
from rumi._write import header_bytes, write_frames

SHORT, LONG, LONG8, DOUBLE, ASCII = 3, 4, 16, 12, 2
TYPE_SIZE = {ASCII: 1, SHORT: 2, LONG: 4, DOUBLE: 8, LONG8: 8}

TILE_OFFSETS, TILE_BYTE_COUNTS = 324, 325

TILE = "b (row h) (col w) -> row col b (h w)"
CELL = "b (row h) (col w) -> row col (b h w)"
CHUNKY = "b (row h) (col w) -> row col (h w b)"

UTM18S = 32718
# The writer reads transform as rasterio's Affine order, (xres, rowrot,
# xorigin, colrot, yres, yorigin), not GDAL's geotransform order.
NORTH_UP = (30.0, 0.0, 500000.0, 0.0, -30.0, 8000000.0)
ROTATED = (30.0, 5.0, 500000.0, 5.0, -30.0, 8000000.0)


def make_frame(shape=(2, 40, 70), tile_size=16, dtype=np.uint16,
               pattern=TILE):
    """Build frames with distinct payload sizes to expose ordering errors."""
    n = np.prod(shape[1:])
    arr = np.arange(shape[0] * n, dtype=dtype).reshape(shape)
    tf = FrameTable.from_array(arr, pattern, tile_size)
    tf["compressed"] = [bytes([i % 251]) * (7 + 3 * i) for i in range(len(tf))]
    return tf


def read_ifd(path):
    """Return ``(entries, next_ifd, bytes)`` from the independent parser."""
    blob = open(path, "rb").read()
    magic, version, reserved = struct.unpack_from("<IHH", blob, 0)
    assert (magic, version, reserved) == (FILE_MAGIC, 1, 0)
    assert blob[:4] == b"RUMI"
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


def test_the_directory_sits_at_sixteen_and_is_the_only_one(tmp_path):
    tf = make_frame()
    path = tmp_path / "a.rumi"
    write_frames(path, tf["compressed"], tf, **labels(tf))
    entries, next_ifd, blob = read_ifd(path)
    assert struct.unpack_from("<Q", blob, 8) == (16,)
    assert next_ifd == 0


def test_tags_ascending(tmp_path):
    tf = make_frame()
    path = tmp_path / "a.rumi"
    write_frames(path, tf["compressed"], tf, **labels(tf),
                 transform=NORTH_UP, crs=UTM18S)
    tags = list(read_ifd(path)[0])
    assert tags == sorted(tags)


def test_tag_set(tmp_path):
    tf = make_frame()
    path = tmp_path / "a.rumi"
    write_frames(path, tf["compressed"], tf, **labels(tf))
    entries = read_ifd(path)[0]

    assert set(entries) == {256, 257, 258, 277, 322, 323, 324, 325, 339,
                            34264, 34735, 65000, 65001}
    # rumi is not TIFF/GeoTIFF: these compatibility fields do not exist.
    assert {259, 262, 284}.isdisjoint(entries)  # compression/photo/planar
    # FrameUnit is rumi's private layout tag.
    assert values(entries[65000], "H") == [0]   # this table is (h w)
    assert values(entries[65001], "I") == [1]   # one time step
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
    path = tmp_path / "a.rumi"
    write_frames(path, tf["compressed"], tf, **labels(tf))
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
    path = tmp_path / "a.rumi"
    write_frames(path, tf["compressed"], tf, **labels(tf))
    entries = read_ifd(path)[0]

    # Both frame tables use physical frame-index order.
    expected = [len(f) for f in tf["compressed"]]
    assert values(entries[TILE_BYTE_COUNTS], "I") == expected
    assert values(entries[TILE_OFFSETS], "Q") == sorted(
        values(entries[TILE_OFFSETS], "Q"))


def test_payload_offsets(tmp_path):
    tf = make_frame()
    path = tmp_path / "a.rumi"
    write_frames(path, tf["compressed"], tf, **labels(tf))
    entries, _next, blob = read_ifd(path)

    base = header_bytes(tf)
    offsets = values(entries[TILE_OFFSETS], "Q")
    counts = values(entries[TILE_BYTE_COUNTS], "I")
    assert min(offsets) == base
    # Magic, version and time fields, one text per band, and no residuals for
    # a single time step.
    trailer = 28 + sum(2 + len(text.encode()) for text in labels(tf)["bands"])
    assert len(blob) == base + sum(counts) + trailer

    for i, (off, n) in enumerate(zip(offsets, counts, strict=True)):
        assert blob[off:off + n] == tf["compressed"][i]


def test_tile_write_order(tmp_path):
    tf = make_frame()
    path = tmp_path / "a.rumi"
    write_frames(path, tf["compressed"], tf, **labels(tf))
    blob = open(path, "rb").read()
    payloads = b"".join(tf["compressed"])
    base = header_bytes(tf)
    assert blob[base:base + len(payloads)] == payloads


def test_north_up_transform(tmp_path):
    tf = make_frame()
    path = tmp_path / "a.rumi"
    write_frames(path, tf["compressed"], tf, **labels(tf),
                 transform=NORTH_UP, crs=UTM18S)
    entries = read_ifd(path)[0]

    # Coefficient 0 is x resolution; coefficient 2 is x origin.
    assert 33550 not in entries and 33922 not in entries
    assert values(entries[34264], "d") == [30.0, 0.0, 0.0, 500000.0,
                                           0.0, -30.0, 0.0, 8000000.0,
                                           0.0, 0.0, 0.0, 0.0,
                                           0.0, 0.0, 0.0, 1.0]


def test_rotated_transform(tmp_path):
    tf = make_frame()
    path = tmp_path / "a.rumi"
    write_frames(path, tf["compressed"], tf, **labels(tf),
                 transform=ROTATED, crs=UTM18S)
    entries = read_ifd(path)[0]

    assert 33550 not in entries and 33922 not in entries
    assert values(entries[34264], "d") == [30.0, 5.0, 0.0, 500000.0,
                                           5.0, -30.0, 0.0, 8000000.0,
                                           0.0, 0.0, 0.0, 0.0,
                                           0.0, 0.0, 0.0, 1.0]


# GeoKey IDs reused by rumi.
FILE_MAGIC = 0x494D5552

MODEL, RASTER, GEOGRAPHIC, PROJECTED = 1024, 1025, 2048, 3072


def geokeys(path):
    """Return the GeoKey header and an ID-to-value mapping."""
    v = values(read_ifd(path)[0][34735], "H")
    return v[:4], {v[4 + i * 4]: v[7 + i * 4] for i in range(v[3])}


def test_geokeys_projected(tmp_path):
    tf = make_frame()
    path = tmp_path / "a.rumi"
    write_frames(path, tf["compressed"], tf, **labels(tf),
                 transform=NORTH_UP, crs=UTM18S)

    head, keys = geokeys(path)
    assert head == [1, 1, 0, 3]
    assert keys == {MODEL: 1, RASTER: 1, PROJECTED: UTM18S}
    # EPSG codes are inline and require no companion parameter tags.
    assert 34736 not in read_ifd(path)[0]
    assert 34737 not in read_ifd(path)[0]


def test_geokeys_geographic(tmp_path):
    tf = make_frame()
    path = tmp_path / "a.rumi"
    write_frames(path, tf["compressed"], tf, **labels(tf), transform=NORTH_UP, crs=4326)
    assert geokeys(path)[1] == {MODEL: 2, RASTER: 1, GEOGRAPHIC: 4326}


def test_geokeys_kind_is_not_the_code_range(tmp_path):
    """EPSG:4037 is projected despite its position among geographic codes."""
    tf = make_frame()
    path = tmp_path / "a.rumi"
    write_frames(path, tf["compressed"], tf, **labels(tf), transform=NORTH_UP, crs=4037)
    assert geokeys(path)[1] == {MODEL: 1, RASTER: 1, PROJECTED: 4037}


def test_pixel_is_point(tmp_path):
    tf = make_frame()
    area, point = tmp_path / "area.rumi", tmp_path / "point.rumi"
    write_frames(area, tf["compressed"], tf, **labels(tf),
                 transform=NORTH_UP, crs=UTM18S)
    write_frames(point, tf["compressed"], tf, **labels(tf),
                 transform=NORTH_UP, crs=UTM18S, pixel_is_point=True)
    assert geokeys(area)[1][RASTER] == 1
    assert geokeys(point)[1][RASTER] == 2


def test_crs_must_be_an_epsg_code(tmp_path):
    tf = make_frame()
    for bad in ("+proj=utm +zone=18 +south", "WGS 84 / UTM zone 18S", 1.5):
        with pytest.raises((ValueError, TypeError), match="EPSG"):
            write_frames(tmp_path / "a.rumi", tf["compressed"], tf, **labels(tf),
                         transform=NORTH_UP, crs=bad)


def test_crs_forms_agree(tmp_path):
    tf = make_frame()
    a, b = tmp_path / "int.rumi", tmp_path / "str.rumi"
    write_frames(a, tf["compressed"], tf, **labels(tf), transform=NORTH_UP, crs=UTM18S)
    write_frames(b, tf["compressed"], tf, **labels(tf),
                 transform=NORTH_UP, crs="EPSG:32718")
    assert a.read_bytes() == b.read_bytes()


def test_unknown_epsg_is_refused(tmp_path):
    tf = make_frame()
    with pytest.raises(ValueError, match="not a projected or geographic"):
        write_frames(tmp_path / "a.rumi", tf["compressed"], tf, **labels(tf),
                     transform=NORTH_UP, crs=999999)


def test_write_blob_round_trip(tmp_path):
    tf = make_frame()
    path, blob = rumi.write(tmp_path / "a.rumi", tf, **labels(tf))

    header = rumi.info(header=blob)
    assert header.shape == (tf.bands, tf.image_length, tf.image_width)
    assert header.dtype == tf.dtype
    assert header.tile == (tf.tile_size, tf.tile_size)
    assert header.frames == len(tf)
    assert _Spec(blob).fields.base_frame_offset == header_bytes(tf)
    assert rumi.info(source=path).header == blob


def test_an_unwritable_path_is_an_os_error_that_names_it(tmp_path):
    # Make the path longer than the old fixed error buffer.
    missing = tmp_path / "missing" / ("d" * 200) / "a.rumi"
    tf = make_frame()
    with pytest.raises(OSError, match="for writing") as raised:
        rumi.write(missing, tf, **labels(tf))
    assert str(missing) in str(raised.value)


def test_edge_tiles(tmp_path):
    tf = make_frame(shape=(3, 257, 256), tile_size=128)
    assert {t.data.shape for t in tf} == {(128, 128), (1, 128)}
    _path, blob = rumi.write(tmp_path / "a.rumi", tf, **labels(tf))
    h = rumi.info(header=blob)
    assert h.shape == (3, 257, 256)
    assert h.frames == len(tf)
    assert h.frame_layout == "h w"


def test_nominal_tile_larger_than_image_round_trips(tmp_path):
    geozl = pytest.importorskip("geozl")
    data = np.arange(3 * 64 * 64, dtype=np.uint16).reshape(3, 64, 64)
    table = rumi.frames(data, CELL, tile_size=16384)
    for frame in table:
        graph = geozl.graph(frame.data, "planar>zigzag>zstd")
        frame.compressed = geozl.compress(frame.data, graph=graph)

    path, header = rumi.write(tmp_path / "clipped.rumi", table, **labels(table))

    assert np.array_equal(rumi.read(path, header), data)
    assert rumi.info(source=path).header == header
    assert rumi.info(header=header).tile == (16384, 16384)


def test_a_unit_must_fit_the_raster(tmp_path):
    """A singleton band axis is omitted from the recorded frame unit."""
    tf = make_frame(shape=(1, 40, 40), tile_size=16, pattern=CELL)
    _path, header = rumi.write(tmp_path / "a.rumi", tf, **labels(tf))
    assert rumi.info(header=header).frame_layout == "h w"

    claim = bytearray(header)
    claim[26] = 1                       # (b h w), which one band cannot be
    with pytest.raises(ValueError, match="frame layout"):
        rumi.info(header=bytes(claim))


def test_a_frame_past_the_size_limit_is_refused():
    """Decoded frame allocation is bounded by a configurable reader limit."""
    from rumi._ffi import lib
    huge = struct.pack("<IHIIIHHHBBBIB", 0x45564F4C, 1, 65535, 65535, 1,
                       65535, 65535, 1, 16, 1, 0, 10, 0)
    assert lib.rumi_get_max_frame_bytes() == 1 << 30
    with pytest.raises(ValueError, match="size limit"):
        rumi.info(header=huge)
    try:
        lib.rumi_set_max_frame_bytes(1 << 40)
        assert rumi.info(header=huge).shape == (1, 65535, 65535)
    finally:
        assert lib.rumi_set_max_frame_bytes(0) == 1 << 30


def test_sub_byte_samples_leave_their_high_bits_zero():
    """Padded sub-byte samples require zero in every unused high bit."""
    ml = pytest.importorskip("ml_dtypes")
    data = np.zeros((2, 8, 8), dtype=ml.float4_e2m1fn)
    assert len(rumi.frames(data, CELL, 4)) == 4

    data.view(np.uint8)[0, 0, 0] = 0xF0
    with pytest.raises(ValueError, match="bits set above"):
        rumi.frames(data, CELL, 4)


def test_the_file_names_its_own_layout(tmp_path):
    """FrameUnit preserves the decoded layout when rebuilding the header."""
    for pattern, want in ((TILE, "h w"), (CELL, "b h w"), (CHUNKY, "h w b")):
        tf = make_frame(shape=(3, 40, 40), tile_size=16, pattern=pattern)
        path, header = rumi.write(tmp_path / "a.rumi", tf, **labels(tf))
        assert rumi.info(header=header).frame_layout == want
        assert rumi.info(source=path).frame_layout == want
        assert rumi.info(source=path).header == header


def test_cell_frame_count(tmp_path):
    """Cell layouts contain one frame per grid position."""
    tf = make_frame(shape=(3, 257, 256), tile_size=128, pattern=CELL)
    _path, blob = rumi.write(tmp_path / "a.rumi", tf, **labels(tf))
    h = rumi.info(header=blob)
    assert h.frame_layout == "b h w"
    assert h.frames == len(tf) == tf.tiles_across * tf.tiles_down


@pytest.mark.parametrize("dtype", [np.uint8, np.int16, np.uint16, np.int32,
                                   np.float32, np.float64])
def test_sample_format(tmp_path, dtype):
    tf = make_frame(shape=(1, 20, 20), tile_size=16, dtype=dtype)
    path = tmp_path / "a.rumi"
    write_frames(path, tf["compressed"], tf, **labels(tf))
    entries = read_ifd(path)[0]

    expected = {np.uint8: 1, np.uint16: 1, np.int16: 2, np.int32: 2,
                np.float32: 3, np.float64: 3}[dtype]
    assert values(entries[339], "H") == [expected]
    assert values(entries[258], "H") == [np.dtype(dtype).itemsize * 8]


# Digests of complete files. Update only for an intentional format change.
GOLDEN = {
    "plain": "0460306b97a4dc3b60a5f619c52161aad139c79a686ca378206a70ce84c103db",
    "north_up": "8ab5ae6e64ff460d854d5ab8d75abb9cebf1b8f6b426bda5387d429be1b8a783",
    "rotated": "c0d4256d41c28750239d2096a215cee073107d9c374ffc689e7a4e68c3a7cdf3",
    "point": "d6f825c2ff6eda562493c14a89f313a04bcad6d063ed8bb6879cec38fc30bdff",
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
    path = tmp_path / "a.rumi"
    write_frames(path, tf["compressed"], tf, **labels(tf), **CASES[case])
    digest = hashlib.sha256(open(path, "rb").read()).hexdigest()
    assert digest == GOLDEN[case]


def test_bad_frame_count(tmp_path):
    tf = make_frame()
    with pytest.raises(ValueError, match="expected"):
        write_frames(tmp_path / "a.rumi", tf["compressed"][:-1], tf, **labels(tf))


def test_empty_payload(tmp_path):
    tf = make_frame()
    frames = list(tf["compressed"])
    frames[3] = b""
    with pytest.raises(ValueError, match="empty payload"):
        write_frames(tmp_path / "a.rumi", frames, tf, **labels(tf))


@pytest.mark.parametrize("shape", [(0, 16, 16), (1, 0, 16), (1, 16, 0)])
def test_frame_dimensions_must_be_positive(shape):
    with pytest.raises(ValueError, match="positive"):
        rumi.frames(np.empty(shape, np.uint8), "b (row h) (col w) -> row col b (h w)", 16)


@pytest.mark.parametrize("dtype", [object, "S1"])
def test_frames_reject_unsupported_dtypes(dtype):
    with pytest.raises(TypeError, match="not supported"):
        rumi.frames(np.zeros((1, 16, 16), dtype=dtype), "b (row h) (col w) -> row col b (h w)", 16)


def test_a_boolean_mask_is_the_binary_type(tmp_path):
    """numpy.bool_ round-trips through rumi's padded binary encoding."""
    geozl = pytest.importorskip("geozl")
    mask = np.arange(1 * 32 * 32).reshape(1, 32, 32) % 3 == 0
    tf = rumi.frames(mask, "b (row h) (col w) -> row col (b h w)", 16)
    assert tf.dtype == np.dtype(bool)
    for f in tf:
        buf = f.data.view(np.uint8)
        f.compressed = geozl.compress(buf, graph=geozl.graph(buf, "planar>zigzag>zstd"))
    path, header = rumi.write(tmp_path / "mask.rumi", tf, **labels(tf))

    h = rumi.info(header=header)
    assert h.dtype is np.bool_
    assert np.array_equal(np.asarray(rumi.read(path, header)), mask)


def test_compressed_frames_must_be_bytes_like():
    tf = make_frame()
    with pytest.raises(TypeError, match="bytes-like"):
        tf[0].compressed = 5
    with pytest.raises(TypeError, match="bytes-like"):
        tf["compressed"] = [5, *tf["compressed"][1:]]


def test_transform_needs_crs(tmp_path):
    tf = make_frame()
    with pytest.raises(ValueError, match="together"):
        write_frames(tmp_path / "a.rumi", tf["compressed"], tf, **labels(tf),
                     transform=NORTH_UP)
    with pytest.raises(ValueError, match="together"):
        write_frames(tmp_path / "a.rumi", tf["compressed"], tf, **labels(tf),
                     crs=UTM18S)


def test_write_needs_all_frames(tmp_path):
    tf = make_frame()
    tf[2].compressed = None
    with pytest.raises(ValueError, match="no payload"):
        rumi.write(tmp_path / "a.rumi", tf, **labels(tf))


def test_every_file_names_its_bands(tmp_path):
    tf = make_frame()
    with pytest.raises(TypeError, match="bands"):
        rumi.write(tmp_path / "a.rumi", tf, time=["2024-01-01"])


@pytest.mark.parametrize("bands, error, because", [
    (None, TypeError, "names its bands"),
    ({"red", "nir"}, TypeError, "must be ordered"),
    (frozenset({"red", "nir"}), TypeError, "must be ordered"),
    (["band 0"], ValueError, "one text per band; got 1"),
    (["band 0", "band 1", "band 2"], ValueError, "one text per band; got more than 2"),
    ("band 0", TypeError, "one text per band"),
    (["band 0", "band 0"], ValueError, "bands 0 and 1 have the same text"),
    (["band 0", ""], ValueError, "band 1 has an empty text"),
    (["band 0", "a\x00b"], ValueError, "NUL"),
    (["band 0", "\ud800"], ValueError, "UTF-8"),
    (["band 0", 7], TypeError, "a band text is a str"),
])
def test_a_band_text_that_cannot_be_stored_writes_nothing(tmp_path, bands,
                                                           error, because):
    tf = make_frame()
    path = tmp_path / "a.rumi"
    with pytest.raises(error, match=because):
        rumi.write(path, tf, bands=bands, time=["2024-01-01"])
    assert not path.exists()


@pytest.mark.parametrize("axis", ["bands", "time"])
def test_writer_stops_reading_an_axis_after_one_extra_entry(tmp_path, axis):
    tf = make_frame()
    count = tf.bands if axis == "bands" else tf.time_count

    def oversized():
        for _ in range(count + 1):
            yield "2024-01-01"
        pytest.fail("writer consumed entries beyond the expected axis length")

    kwargs = labels(tf)
    kwargs[axis] = oversized()
    path = tmp_path / "a.rumi"
    with pytest.raises(ValueError, match=f"; got more than {count}$"):
        rumi.write(path, tf, **kwargs)
    assert not path.exists()


@pytest.mark.parametrize("bands, count", [([], "0"), (["red", "nir"], "more than 1")])
def test_band_count_error_uses_singular_for_one_band(tmp_path, bands, count):
    tf = make_frame(shape=(1, 16, 16))
    message = f"a file with 1 band needs one text per band; got {count}"
    with pytest.raises(ValueError, match=f"^{message}$"):
        rumi.write(tmp_path / "a.rumi", tf, bands=bands, time=["2024-01-01"])


@pytest.mark.parametrize("container", [set, frozenset])
def test_writer_rejects_unordered_time(tmp_path, container):
    tf = make_frame()
    with pytest.raises(TypeError, match="must be ordered"):
        rumi.write(tmp_path / "a.rumi", tf, bands=["red", "nir"],
                   time=container(["2024-01-01"]))


def test_writer_preserves_generator_order(tmp_path):
    tf = make_frame()
    path, _ = rumi.write(tmp_path / "a.rumi", tf,
                         bands=(b for b in ["nir", "red"]),
                         time=(t for t in ["2024-01-01"]))
    assert rumi.info(source=path).bands == ["nir", "red"]


def test_a_transform_is_six_coefficients(tmp_path):
    """Reject transforms missing any of the six affine coefficients."""
    tf = make_frame()
    with pytest.raises(ValueError, match="six coefficients"):
        write_frames(tmp_path / "a.rumi", tf["compressed"], tf, **labels(tf),
                     transform=(10.0, 0.0, 3e5, 0.0, -10.0), crs=UTM18S)
    # Affine objects may expose additional values after the six coefficients.
    write_frames(tmp_path / "b.rumi", tf["compressed"], tf, **labels(tf),
                 transform=(10.0, 0.0, 3e5, 0.0, -10.0, 8.1e6, 0.0, 0.0, 1.0),
                 crs=UTM18S)
