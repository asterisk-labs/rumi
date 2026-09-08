"""Cube layouts, frame order, time coordinates, and selections."""

import numpy as np
import pytest
import rumi

geozl = pytest.importorskip("geozl")

T, B, Y, X = 4, 3, 40, 40
TILE = 16
ACROSS = DOWN = 3  # ceil(40 / 16)

# Every frame unit valid when B > 1 and T > 1.
LAYOUTS = {
    "tile_bt": ("t b (row h) (col w) -> row col b t (h w)", 0, "h w"),
    "planar":  ("t b (row h) (col w) -> row col (b t h w)", 3, "b t h w"),
    "planar_t": ("t b (row h) (col w) -> row col (t b h w)", 4, "t b h w"),
    "band_planar": ("t b (row h) (col w) -> row col (b h w t)", 5, "b h w t"),
    "time_planar": ("t b (row h) (col w) -> row col (t h w b)", 6, "t h w b"),
    "chunky_bt": ("t b (row h) (col w) -> row col (h w b t)", 7, "h w b t"),
    "chunky_tb": ("t b (row h) (col w) -> row col (h w t b)", 8, "h w t b"),
    "tile_tb": ("t b (row h) (col w) -> row col t b (h w)", 9, "h w"),
}


def cube():
    return np.arange(T * B * Y * X, dtype=np.uint16).reshape(T, B, Y, X)


def store(tmp_path, name, arr=None, tile=TILE, time=None, stem=None):
    pattern = LAYOUTS[name][0] if name in LAYOUTS else name
    tf = rumi.frames(cube() if arr is None else arr, pattern, tile)
    for f in tf:
        f.compressed = geozl.compress(
            f.data, graph=geozl.graph(f.data, "planar>zigzag>zstd"))
    stem = stem or name.replace(" ", "_")[:40]
    return tf, *rumi.write(tmp_path / f"{stem}.rumi", tf, time=time)


@pytest.mark.parametrize("name", list(LAYOUTS))
def test_every_layout_round_trips(name, tmp_path):
    _tf, path, header = store(tmp_path, name)
    assert np.array_equal(np.asarray(rumi.read(path, header)), cube())


@pytest.mark.parametrize("name", list(LAYOUTS))
def test_the_file_records_the_layout_it_was_given(name, tmp_path):
    _pattern, unit, layout = LAYOUTS[name]
    tf, _path, header = store(tmp_path, name)
    h = rumi.info(header=header)
    assert tf.frame_unit == unit
    assert h.frame_layout == layout
    assert h.shape == (T, B, Y, X)
    assert h.time_count == T


def test_a_cell_holds_every_step_and_a_tile_holds_one(tmp_path):
    cells = store(tmp_path, "planar")[0]
    tiles = store(tmp_path, "tile_bt")[0]
    assert len(cells) == ACROSS * DOWN
    assert len(tiles) == ACROSS * DOWN * B * T
    assert cells[0].data.shape == (B, T, TILE, TILE)
    assert tiles[0].data.shape == (TILE, TILE)


def test_the_two_index_orders_are_different_files(tmp_path):
    """Units 0 and 9 differ only in their band/time frame order."""
    bt, path_bt, head_bt = store(tmp_path, "tile_bt")
    tb, path_tb, head_tb = store(tmp_path, "tile_tb")

    assert len(bt) == len(tb)
    assert rumi.info(header=head_bt).frames == rumi.info(header=head_tb).frames
    assert rumi.info(header=head_bt).index_order == ("b", "t")
    assert rumi.info(header=head_tb).index_order == ("t", "b")
    assert path_bt.read_bytes() != path_tb.read_bytes()

    # Frame 1 is the next step of band 0 in one and the next band of step 0 in
    # the other, so the samples they hold disagree.
    assert bt[1].band == 0 and bt[1].time == 1
    assert tb[1].band == 1 and tb[1].time == 0


@pytest.mark.parametrize("name, order", [("tile_bt", ("b", "t")),
                                         ("tile_tb", ("t", "b"))])
def test_the_index_is_the_one_the_spec_defines(name, order, tmp_path):
    """Check frame indices against the formula in the specification."""
    tf, _path, _header = store(tmp_path, name)
    for k in range(len(tf)):
        f = tf[k]
        spatial = f.row * ACROSS + f.col
        outer, inner = ((f.band, B), (f.time, T)) if order == ("b", "t") \
            else ((f.time, T), (f.band, B))
        assert (spatial * outer[1] + outer[0]) * inner[1] + inner[0] == k


@pytest.mark.parametrize("name", list(LAYOUTS))
def test_a_frame_holds_the_samples_its_position_claims(name, tmp_path):
    """Validate each frame's labels against the samples it contains."""
    src = cube()
    tf, _path, _header = store(tmp_path, name)
    axes = tf.layout.split()
    for k in (0, 1, len(tf) // 2, len(tf) - 1):
        f = tf[k]
        ys, xs = f.row * TILE, f.col * TILE
        want = src[:, :, ys:ys + f.data.shape[axes.index("h")],
                   xs:xs + f.data.shape[axes.index("w")]]
        want = want[slice(None) if "t" in axes else f.time,
                    slice(None) if "b" in axes else f.band]
        held = [a for a in ("b", "t") if a in axes] + ["h", "w"]
        # Convert the filtered (t, b, h, w) slice to canonical b/t order.
        if "b" in axes and "t" in axes:
            want = want.transpose(1, 0, 2, 3)
        assert np.array_equal(f.data,
                              want.transpose([held.index(a) for a in axes]))


@pytest.mark.parametrize("name", list(LAYOUTS))
def test_a_selection_reaches_the_right_steps(name, tmp_path):
    src = cube()
    _tf, path, header = store(tmp_path, name)
    for kw, want in [({"time": [0, 2]}, src[[0, 2]]),
                     ({"time": [3, 1]}, src[[3, 1]]),
                     ({"time": [1], "bands": [2, 0]}, src[[1]][:, [2, 0]]),
                     ({"time": [2], "window": (4, 4, 16, 32)},
                      src[[2]][:, :, 4:20, 4:36])]:
        got = np.asarray(rumi.read(path, header, **kw))
        assert np.array_equal(got, want), kw


def test_named_selection_reaches_the_same_cube_window(tmp_path):
    src = cube()
    _tf, path, header = store(tmp_path, "planar")
    got = rumi.read(path, header, time=(1, 3), bands=[2, 0],
                    window=(4, 4, 16, 32))
    assert np.array_equal(np.asarray(got), src[1:3][:, [2, 0], 4:20, 4:36])


def test_a_step_out_of_range_is_refused(tmp_path):
    _tf, path, header = store(tmp_path, "planar")
    with pytest.raises(ValueError, match="out of"):
        rumi.read(path, header, time=[T])


def test_an_output_pattern_may_place_time(tmp_path):
    src = cube()
    _tf, path, header = store(tmp_path, "planar")
    assert np.array_equal(np.asarray(rumi.read(path, header, pattern="b t y x")),
                          src.transpose(1, 0, 2, 3))
    assert np.array_equal(np.asarray(rumi.read(path, header, pattern="y x t b")),
                          src.transpose(2, 3, 0, 1))
    assert np.array_equal(np.asarray(rumi.read(path, header, pattern="(t b) y x")),
                          src.reshape(T * B, Y, X))


def test_a_cube_defaults_to_carrying_its_time_axis(tmp_path):
    """Selecting one Cube step preserves the time axis."""
    _tf, path, header = store(tmp_path, "planar")
    assert np.asarray(rumi.read(path, header)).shape == (T, B, Y, X)
    assert np.asarray(rumi.read(path, header, time=[1])).shape == (1, B, Y, X)
    assert np.asarray(rumi.read(path, header, bands=[1])).shape == (T, 1, Y, X)


def test_one_step_is_an_image_whatever_the_pattern_named(tmp_path):
    """A time axis of length one is omitted from the recorded frame unit."""
    flat = np.arange(2 * 30 * 30, dtype=np.uint16).reshape(1, 2, 30, 30)
    tf, path, header = store(tmp_path, "planar", arr=flat)
    assert tf.frame_unit == 1
    assert rumi.info(header=header).frame_layout == "b h w"
    assert rumi.info(header=header).shape == (2, 30, 30)
    assert np.array_equal(np.asarray(rumi.read(path, header)), flat[0])


def test_a_single_band_cube_places_time_where_bands_would_go(tmp_path):
    """Units 1 and 2 resolve their non-spatial axis from B and T."""
    one = np.arange(4 * 30 * 30, dtype=np.uint16).reshape(4, 1, 30, 30)
    tf, path, header = store(tmp_path, "planar", arr=one)
    assert tf.frame_unit == 1
    assert rumi.info(header=header).frame_layout == "t h w"
    assert np.array_equal(np.asarray(rumi.read(path, header)), one)


def test_ragged_edges_survive_a_time_axis(tmp_path):
    rag = np.arange(3 * 2 * 100 * 130, dtype=np.uint16).reshape(3, 2, 100, 130)
    for name in ("planar", "tile_tb", "chunky_bt"):
        _tf, path, header = store(tmp_path, name, arr=rag, tile=32)
        assert np.array_equal(np.asarray(rumi.read(path, header)), rag)
        got = rumi.read(path, header, time=[2, 0], bands=[1],
                        window=(30, 90, 40, 40))
        assert np.array_equal(np.asarray(got),
                              rag[[2, 0]][:, [1]][:, :, 30:70, 90:130])


def test_a_cube_carries_its_dates(tmp_path):
    days = ["2024-05-01", "2024-06-01", "2024-07-01", "2024-08-01"]
    _tf, path, _header = store(tmp_path, "planar", time=days)
    import datetime as dt
    assert rumi.info(source=path).time == [dt.date.fromisoformat(d) for d in days]


def test_a_date_list_covers_every_step(tmp_path):
    with pytest.raises(ValueError, match="one entry per time step"):
        store(tmp_path, "planar", time=["2024-05-01"])


def test_a_batch_of_cubes_is_rank_five(tmp_path):
    src = cube()
    made = [store(tmp_path, "planar", arr=src + 10000 * i, stem=f"s{i}")[1:]
            for i in range(3)]
    paths = [p for p, _h in made]
    heads = [h for _p, h in made]
    want = np.stack([src + 10000 * i for i in range(3)])
    windows = [(0, 0, Y, X)] * len(paths)
    assert np.array_equal(
        np.asarray(rumi.read_many(paths, heads, windows=windows)), want)
    got = rumi.read_many(paths, heads, windows=windows, time=[1])
    assert np.array_equal(np.asarray(got), want[:, [1]])

    named = rumi.read_many(
        paths, heads, windows=[(4, 4, 16, 32)] * len(paths),
        time=[1], bands=[2, 0])
    assert np.array_equal(np.asarray(named),
                          want[:, [1]][:, :, [2, 0], 4:20, 4:36])


def test_the_table_names_the_axes_the_index_walks(tmp_path):
    tiles = store(tmp_path, "tile_tb")[0]
    cells = store(tmp_path, "planar")[0]
    assert tiles.dims == ("row", "col", "time", "band")
    assert cells.dims == ("row", "col")
    assert tiles[5].tile == f"{tiles[5].time}.{tiles[5].band}.0.0"
    with pytest.raises(AttributeError, match="holds every axis"):
        _ = cells[0].tile
    with pytest.raises(AttributeError, match="every time step"):
        _ = cells[0].time


def test_a_selection_shortens_the_axis_it_selects(tmp_path):
    """Selection length changes an axis extent without removing the axis."""
    src = cube()
    _tf, path, header = store(tmp_path, "planar")
    for kw, want in (({}, src), ({"time": [1]}, src[[1]]),
                     ({"time": [2, 0]}, src[[2, 0]]),
                     ({"time": [1], "bands": [0]}, src[[1]][:, [0]])):
        got = rumi.read(path, header, **kw)
        assert got.shape == want.shape, kw
        assert np.asarray(got).shape == want.shape, kw
        assert np.array_equal(np.asarray(got), want), kw


def test_a_cell_value_reaches_every_frame_of_its_cell(tmp_path):
    """Per-cell metadata repeats across all frames at one grid position."""
    tiles, _p, _h = store(tmp_path, "tile_bt")
    cells = ACROSS * DOWN
    assert tiles._per_cell == B * T
    tiles.attach("scene", [f"s{i}" for i in range(cells)])
    for k in range(len(tiles)):
        assert tiles[k].scene == f"s{tiles[k].row * ACROSS + tiles[k].col}"


@pytest.mark.parametrize("name, order", [("tile_bt", ("b", "t")),
                                         ("tile_tb", ("t", "b"))])
def test_planned_ranges_come_in_frame_index_order(name, order, tmp_path):
    """Planned ranges follow frame-index order for units 0 and 9."""
    from rumi._ffi import _Spec, ffi, lib
    tf, _path, header = store(tmp_path, name)
    assert rumi.info(header=header).index_order == order

    spec = _Spec(header)
    out = ffi.new("rumi_range**")
    count = ffi.new("size_t*")
    assert lib.rumi_plan_ranges(spec.handle, ffi.NULL, 0, ffi.NULL, 0,
                                0, TILE, 0, TILE, out, count) == lib.RUMI_OK
    got = [out[0][i].offset for i in range(count[0])]
    lib.rumi_free(out[0])

    at = int(_Spec(header).fields.base_frame_offset)
    physical = []
    for k, payload in enumerate(tf["compressed"]):
        if tf[k].row == 0 and tf[k].col == 0:
            physical.append(at)
        at += len(payload)
    assert got == physical


def test_planned_ranges_do_not_follow_the_selection(tmp_path):
    """Range order and uniqueness do not depend on selection order."""
    from rumi._ffi import _Spec, ffi, lib
    _tf, _path, header = store(tmp_path, "tile_bt")
    spec = _Spec(header)
    out = ffi.new("rumi_range**")
    count = ffi.new("size_t*")

    def plan(times, bands):
        t = ffi.new("int[]", times)
        b = ffi.new("int[]", bands)
        assert lib.rumi_plan_ranges(spec.handle, t, len(times), b, len(bands),
                                    0, TILE, 0, TILE, out, count) == lib.RUMI_OK
        got = [out[0][i].offset for i in range(count[0])]
        lib.rumi_free(out[0])
        return got

    forward = plan([1, 2], [1, 2])
    assert forward == sorted(forward)
    assert plan([2, 1], [2, 1]) == forward          # reordered, same answer
    assert plan([1, 1], [2, 2]) == plan([1], [2])   # repeats fetch once
