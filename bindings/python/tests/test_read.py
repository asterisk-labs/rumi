"""The read path, over both sources.

A window read has to give the same pixels whether the bytes came off disk or out
of a buffer, and plan_ranges has to name exactly the bytes that window touches.
"""

import numpy as np
import pytest

import rumi
from rumi._ffi import _Spec, ffi, lib

geozl = pytest.importorskip("geozl")

GRAPH = "planar>zigzag>zstd"


@pytest.fixture(scope="module")
def image(tmp_path_factory):
    """A small scene written once, with edge tiles."""
    rng = np.random.default_rng(0)
    data = rng.integers(0, 3000, (3, 100, 130)).astype(np.uint16)
    tf = rumi.frames(data, 32)
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
    rc = lib.rumi_plan_ranges(spec.handle, b, n_b,
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
    """The point of the header: a caller can fetch the tiles a window needs and
    read the window back out of a buffer that is mostly holes."""
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
    """The same scene with one frame per grid position."""
    rng = np.random.default_rng(1)
    data = rng.integers(0, 3000, (5, 100, 130)).astype(np.uint16)
    tf = rumi.frames(data, 32, unit="cell")
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
    """One frame holds every band, so the order comes from how the planes are
    handed out."""
    path, header, data = cell_image
    assert np.array_equal(rumi.read(path, header, b=bands), data[bands])


@pytest.mark.parametrize("bands", [[0], [3, 1], [0, 1, 2, 3, 4]])
def test_a_cell_window_matches_a_tile_window(tmp_path, bands):
    """Two ways to store one image, so a window reads the same from either."""
    rng = np.random.default_rng(2)
    data = rng.integers(0, 3000, (5, 100, 130)).astype(np.uint16)
    out = {}
    for unit in ("tile", "cell"):
        tf = rumi.frames(data, 32, unit=unit)
        graphs = {}
        for t in tf:
            g = graphs.get(t.data.shape)
            if g is None:
                g = graphs[t.data.shape] = geozl.graph(t.data, GRAPH)
            t.compressed = geozl.compress(t.data, graph=g)
        path, header = rumi.write(tmp_path / f"{unit}.rumi", tf)
        out[unit] = np.asarray(
            rumi.read(str(path), header, b=bands, y=(30, 70), x=(20, 90)))
    assert np.array_equal(out["tile"], out["cell"])
    assert np.array_equal(out["cell"], data[bands, 30:70, 20:90])


def test_a_cell_read_decodes_each_frame_once(cell_image):
    """A cell read must not scale with the bands asked for. It did: the plan
    made one task per band and each decoded the whole frame."""
    path, header, _data = cell_image
    one = plan(header, bands=[0], y=(0, 100), x=(0, 130))
    all_five = plan(header, bands=[0, 1, 2, 3, 4], y=(0, 100), x=(0, 130))
    assert len(one) == len(all_five)


def test_a_tile_read_still_asks_per_band(image):
    """A tile frame holds one band, so tile reads do scale."""
    _path, header, _data = image
    one = plan(header, bands=[0], y=(0, 100), x=(0, 130))
    both = plan(header, bands=[0, 2], y=(0, 100), x=(0, 130))
    assert len(both) == 2 * len(one)


def test_a_cell_stack_round_trips(tmp_path):
    """A stack merges one plan per image."""
    rng = np.random.default_rng(3)
    base = rng.integers(0, 3000, (4, 64, 64)).astype(np.uint16)
    paths, headers, cubes = [], [], []
    for i in range(3):
        cube = (base + i * 100).astype(np.uint16)
        tf = rumi.frames(cube, 32, unit="cell")
        for t in tf:
            t.compressed = geozl.compress(t.data,
                                          graph=geozl.graph(t.data, GRAPH))
        path, header = rumi.write(tmp_path / f"s{i}.rumi", tf)
        paths.append(str(path))
        headers.append(header)
        cubes.append(cube)
    got = np.asarray(rumi.read(paths, headers, b=[3, 0]))
    assert np.array_equal(got, np.stack([c[[3, 0]] for c in cubes]))
