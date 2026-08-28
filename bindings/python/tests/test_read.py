"""Read paths for local files, memory buffers, windows, and stacks."""

import numpy as np
import pytest
import rumi
from rumi._ffi import _Spec, ffi, lib

geozl = pytest.importorskip("geozl")

GRAPH = "planar>zigzag>zstd"

PATTERNS = {"tile": "b (row h) (col w) -> row col b (h w)",
            "cell": "b (row h) (col w) -> row col (b h w)",
            "chunky": "b (row h) (col w) -> row col (h w b)"}


@pytest.fixture(scope="module")
def image(tmp_path_factory):
    """A small scene written once, with edge tiles."""
    rng = np.random.default_rng(0)
    data = rng.integers(0, 3000, (3, 100, 130)).astype(np.uint16)
    tf = rumi.frames(data, "b (row h) (col w) -> row col b (h w)", 32)
    graphs = {}
    for t in tf:
        g = graphs.get(t.data.shape)
        if g is None:
            g = graphs[t.data.shape] = geozl.graph(t.data, GRAPH)
        t.compressed = geozl.compress(t.data, graph=g)
    path = tmp_path_factory.mktemp("read") / "img.rumi"
    path, header = rumi.write(path, tf)
    return str(path), header, data


def test_path_round_trip(image):
    path, header, data = image
    assert np.array_equal(rumi.read(path, header), data)


def test_bytes_round_trip(image):
    path, header, data = image
    blob = open(path, "rb").read()
    assert np.array_equal(rumi.read(blob, header), data)


def test_both_sources_agree(image):
    path, header, data = image
    blob = open(path, "rb").read()
    window = dict(b=[0, 2], y=(10, 74), x=(30, 94))
    assert np.array_equal(rumi.read(path, header, **window),
                          rumi.read(blob, header, **window))


def test_named_selection_matches_the_short_form(image):
    path, header, _data = image
    short = rumi.read(path, header, b=[0, 2], y=(10, 74), x=(30, 94))
    named = rumi.read(path, header, bands=[0, 2],
                      window=(10, 30, 64, 64))
    assert np.array_equal(named, short)


@pytest.mark.parametrize("kw, message", [
    ({"time": [0], "t": [0]}, "use time or t"),
    ({"bands": [0], "b": [0]}, "use bands or b"),
    ({"window": (0, 0, 1, 1), "y": (0, 1)}, "use window or y/x"),
    ({"window": (0, 0, 1, 1), "x": (0, 1)}, "use window or y/x"),
])
def test_named_and_short_selection_cannot_be_mixed(image, kw, message):
    path, header, _data = image
    with pytest.raises(ValueError, match=message):
        rumi.read(path, header, **kw)


@pytest.mark.parametrize("window, error", [
    ((0, 0, 32), TypeError),
    ([0, 0, 32, 32], TypeError),
    ((0, 0, "32", 32), TypeError),
    ((-1, 0, 32, 32), ValueError),
    ((0, 0, 0, 32), ValueError),
])
def test_a_window_has_an_origin_and_positive_size(image, window, error):
    path, header, _data = image
    with pytest.raises(error, match="window"):
        rumi.read(path, header, window=window)


def test_a_named_window_stays_inside_the_image(image):
    path, header, _data = image
    with pytest.raises(ValueError, match="out of"):
        rumi.read(path, header, window=(90, 120, 20, 20))


def test_memoryview_and_bytearray(image):
    path, header, data = image
    blob = open(path, "rb").read()
    for form in (memoryview(blob), bytearray(blob)):
        assert np.array_equal(rumi.read(form, header), data)


def test_bytes_need_the_header(image):
    path, header, _data = image
    blob = open(path, "rb").read()
    with pytest.raises(ValueError, match="header"):
        rumi.read(blob)


def test_stack_mixes_paths_and_bytes(image):
    path, header, data = image
    blob = open(path, "rb").read()
    out = rumi.read([path, blob, path], [header] * 3)
    assert out.shape == (3, *data.shape)
    assert np.array_equal(out[1], data)


def test_truncated_buffer_is_refused(image):
    path, header, _data = image
    blob = open(path, "rb").read()
    with pytest.raises(ValueError, match="frame data needs"):
        rumi.read(blob[:len(blob) // 2], header)


def test_missing_file(tmp_path, image):
    _path, header, _data = image
    with pytest.raises(OSError, match="could not open"):
        rumi.read(tmp_path / "gone.rumi", header)


def plan(header, *, bands=None, y=(0, 0), x=(0, 0)):
    """Bands 0-based here, like the Python API; C wants them 1-based."""
    spec = _Spec(header)
    b = ffi.NULL if bands is None else ffi.new("int[]", [i + 1 for i in bands])
    n_b = 0 if bands is None else len(bands)
    out = ffi.new("rumi_range**")
    count = ffi.new("size_t*")
    rc = lib.rumi_plan_ranges(spec.handle, ffi.NULL, 0, b, n_b,
                              y[0], y[1] - y[0], x[0], x[1] - x[0], out, count)
    assert rc == lib.RUMI_OK
    try:
        return [(out[0][i].offset, out[0][i].length) for i in range(count[0])]
    finally:
        lib.rumi_free(out[0])


def test_plan_ranges_covers_one_tile(image):
    path, header, _data = image
    h = rumi.RumiHeader(header).to_dict()
    ranges = plan(header, bands=[0], y=(0, 1), x=(0, 1))
    assert len(ranges) == 1
    assert ranges[0][0] == h["base_frame_offset"]


def test_plan_ranges_counts_frames_for_selected_bands(image):
    path, header, _data = image
    ranges = plan(header, bands=[0, 2], y=(0, 64), x=(0, 64))
    assert len(ranges) == 2 * 2 * 2  # 2 rows, 2 cols, 2 bands


def test_plan_ranges_is_a_subset_of_the_file(image):
    path, header, _data = image
    size = len(open(path, "rb").read())
    full = plan(header, y=(0, 100), x=(0, 130))
    window = plan(header, bands=[0], y=(0, 32), x=(0, 32))
    assert sum(n for _o, n in window) < sum(n for _o, n in full) < size


def test_plan_ranges_points_at_real_frames(image):
    path, header, _data = image
    blob = open(path, "rb").read()
    for offset, length in plan(header, bands=[0], y=(0, 32), x=(0, 32)):
        frame = blob[offset:offset + length]
        assert len(frame) == length
        assert len(geozl.decompress(frame)) > 0


def test_fetching_only_the_planned_ranges_is_enough(image):
    """Planned ranges are sufficient to read a window from sparse memory."""
    path, header, data = image
    blob = open(path, "rb").read()
    window = dict(b=[0], y=(0, 32), x=(0, 32))

    base = rumi.RumiHeader(header).to_dict()["base_frame_offset"]
    sparse = bytearray(len(blob))
    sparse[:base] = blob[:base]
    fetched = 0
    for offset, length in plan(header, bands=[0], y=(0, 32), x=(0, 32)):
        sparse[offset:offset + length] = blob[offset:offset + length]
        fetched += length

    assert fetched < len(blob) / 4
    assert np.array_equal(rumi.read(bytes(sparse), header, **window),
                          rumi.read(path, header, **window))


@pytest.fixture(scope="module")
def cell_image(tmp_path_factory):
    """Return the same scene with one cell frame per grid position."""
    rng = np.random.default_rng(1)
    data = rng.integers(0, 3000, (5, 100, 130)).astype(np.uint16)
    tf = rumi.frames(data, "b (row h) (col w) -> row col (b h w)", 32)
    graphs = {}
    for t in tf:
        g = graphs.get(t.data.shape)
        if g is None:
            g = graphs[t.data.shape] = geozl.graph(t.data, GRAPH)
        t.compressed = geozl.compress(t.data, graph=g)
    path = tmp_path_factory.mktemp("cell") / "img.rumi"
    path, header = rumi.write(path, tf)
    return str(path), header, data


def test_a_cell_file_round_trips(cell_image):
    path, header, data = cell_image
    assert np.array_equal(rumi.read(path, header), data)


@pytest.mark.parametrize("bands", [[0], [4], [0, 4], [4, 3, 2, 1, 0], [2, 2],
                                   [0, 0, 3], [0, 1, 2, 3, 4]])
def test_a_cell_read_keeps_the_band_order_asked_for(cell_image, bands):
    """A cell frame preserves the requested band order."""
    path, header, data = cell_image
    assert np.array_equal(rumi.read(path, header, b=bands), data[bands])


@pytest.mark.parametrize("bands", [[0], [3, 1], [0, 1, 2, 3, 4]])
def test_every_layout_reads_the_same_window(tmp_path, bands):
    """Every Image frame layout decodes to the same logical window."""
    rng = np.random.default_rng(2)
    data = rng.integers(0, 3000, (5, 100, 130)).astype(np.uint16)
    out = {}
    for unit, pattern in PATTERNS.items():
        tf = rumi.frames(data, pattern, 32)
        graphs = {}
        for t in tf:
            g = graphs.get(t.data.shape)
            if g is None:
                g = graphs[t.data.shape] = geozl.graph(t.data, GRAPH)
            t.compressed = geozl.compress(t.data, graph=g)
        path, header = rumi.write(tmp_path / f"{unit}.rumi", tf)
        out[unit] = np.asarray(
            rumi.read(str(path), header, b=bands, y=(30, 70), x=(20, 90)))
    want = data[bands, 30:70, 20:90]
    for unit, got in out.items():
        assert np.array_equal(got, want), unit


def test_a_chunky_frame_holds_the_pixel_spectrum(tmp_path):
    """An ``h w b`` frame decodes to the logical ``B Y X`` Image."""
    rng = np.random.default_rng(3)
    data = rng.integers(0, 3000, (4, 70, 90)).astype(np.uint16)
    tf = rumi.frames(data, PATTERNS["chunky"], 32)
    assert tf[0].data.shape == (32, 32, 4)
    assert tf[-1].data.shape == (6, 26, 4)          # the corner, cut to bounds
    for t in tf:
        t.compressed = geozl.compress(t.data, graph=geozl.graph(t.data, GRAPH))
    path, header = rumi.write(tmp_path / "chunky.rumi", tf)
    assert rumi.RumiHeader(header).frame_unit == "h w b"
    assert np.array_equal(rumi.read(str(path), header), data)


def _write(tmp_path, name, data, unit, tile=16):
    tf = rumi.frames(data, PATTERNS[unit], tile)
    for t in tf:
        t.compressed = geozl.compress(t.data, graph=geozl.graph(t.data, GRAPH))
    path, header = rumi.write(tmp_path / f"{name}.rumi", tf)
    return str(path), header


@pytest.mark.parametrize("unit", list(PATTERNS))
def test_a_file_names_its_own_layout(tmp_path, unit):
    """FrameUnit preserves layout when the external header is rebuilt."""
    rng = np.random.default_rng(5)
    data = rng.integers(0, 3000, (4, 70, 90)).astype(np.uint16)
    path, header = _write(tmp_path, unit, data, unit, tile=32)
    assert np.array_equal(rumi.read(path, header), data)
    assert np.array_equal(rumi.read(path), data)
    assert (rumi.RumiHeader.from_path(path).frame_unit
            == rumi.RumiHeader(header).frame_unit)


def test_a_stack_needs_no_headers(tmp_path):
    """A stack may rebuild headers for files with different frame layouts."""
    rng = np.random.default_rng(6)
    data = rng.integers(0, 3000, (4, 70, 90)).astype(np.uint16)
    paths = [_write(tmp_path, u, data, u, tile=32)[0] for u in ("cell", "chunky")]
    got = np.asarray(rumi.read(paths))
    assert np.array_equal(got[0], data) and np.array_equal(got[1], data)


def test_a_chunky_read_decodes_each_frame_once(tmp_path):
    """Cell frames decode once per selected grid position."""
    rng = np.random.default_rng(4)
    data = rng.integers(0, 3000, (5, 100, 130)).astype(np.uint16)
    tf = rumi.frames(data, PATTERNS["chunky"], 32)
    for t in tf:
        t.compressed = geozl.compress(t.data, graph=geozl.graph(t.data, GRAPH))
    _path, header = rumi.write(tmp_path / "chunky.rumi", tf)
    one = plan(header, bands=[0], y=(0, 100), x=(0, 130))
    all_five = plan(header, bands=[0, 1, 2, 3, 4], y=(0, 100), x=(0, 130))
    assert len(one) == len(all_five) == len(tf)


def test_a_cell_read_decodes_each_frame_once(cell_image):
    """Selecting more bands from a cell must not duplicate decode tasks."""
    path, header, _data = cell_image
    one = plan(header, bands=[0], y=(0, 100), x=(0, 130))
    all_five = plan(header, bands=[0, 1, 2, 3, 4], y=(0, 100), x=(0, 130))
    assert len(one) == len(all_five)


def test_a_tile_read_still_asks_per_band(image):
    """Tile layouts require one decode task per selected band."""
    _path, header, _data = image
    one = plan(header, bands=[0], y=(0, 100), x=(0, 130))
    both = plan(header, bands=[0, 2], y=(0, 100), x=(0, 130))
    assert len(both) == 2 * len(one)


def test_a_cell_stack_round_trips(tmp_path):
    """Stack reads merge one decode plan per selected image."""
    rng = np.random.default_rng(3)
    base = rng.integers(0, 3000, (4, 64, 64)).astype(np.uint16)
    paths, headers, cubes = [], [], []
    for i in range(3):
        cube = (base + i * 100).astype(np.uint16)
        tf = rumi.frames(cube, "b (row h) (col w) -> row col (b h w)", 32)
        for t in tf:
            t.compressed = geozl.compress(t.data,
                                          graph=geozl.graph(t.data, GRAPH))
        path, header = rumi.write(tmp_path / f"s{i}.rumi", tf)
        paths.append(str(path))
        headers.append(header)
        cubes.append(cube)
    got = np.asarray(rumi.read(paths, headers, b=[3, 0]))
    assert np.array_equal(got, np.stack([c[[3, 0]] for c in cubes]))
