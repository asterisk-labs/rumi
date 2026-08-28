"""Tile-aligned sample plans."""

import numpy as np
import pytest
import rumi

geozl = pytest.importorskip("geozl")

T, B, Y, X = 3, 3, 40, 50
TILE = 16
TILED = "t b (row h) (col w) -> row col b t (h w)"
TILED_TB = "t b (row h) (col w) -> row col t b (h w)"
CELL = "t b (row h) (col w) -> row col (b t h w)"


def store(tmp_path, pattern=TILED):
    data = np.arange(T * B * Y * X, dtype=np.uint16).reshape(T, B, Y, X)
    frames = rumi.frames(data, pattern, TILE)
    for frame in frames:
        frame.compressed = geozl.compress(
            frame.data, graph=geozl.graph(frame.data, "planar>zigzag>zstd"))
    path, header = rumi.write(tmp_path / "samples.rumi", frames)
    return data, path, header


def expected(data, selection):
    y, x, height, width = selection["window"]
    start, stop = selection["time"]
    bands = selection["bands"]
    if bands is None:
        bands = slice(None)
    elif isinstance(bands, tuple):
        bands = slice(*bands)
    return data[start:stop, bands, y:y + height, x:x + width]


def test_defaults_make_one_sample_per_tile_and_step(tmp_path):
    data, path, header = store(tmp_path)
    planned = rumi.chunks(header)

    assert len(planned) == 2 * 3 * T
    assert repr(planned) == "<rumi.chunks 18 samples>"
    assert planned[0] == {
        "window": (0, 0, TILE, TILE),
        "time": (0, 1),
        "bands": None,
    }
    assert planned[3] == {
        "window": (0, TILE, TILE, TILE),
        "time": (0, 1),
        "bands": None,
    }
    got = rumi.read(path, header, **planned[3])
    assert np.array_equal(got, data[0:1, :, 0:16, 16:32])


@pytest.mark.parametrize("pattern", [TILED, TILED_TB])
@pytest.mark.parametrize(
    ("options", "indexes"),
    [
        ({}, [0, 3, -1]),
        ({"tiles": 2, "bands": [2, 0]}, [0, 1, -1]),
        ({"tiles": (1, 2), "time": 2, "bands": (1, 3)}, [0, -1]),
        ({"tiles": 2, "time": 2, "bands": [0, 2], "edge": "clip"},
         [0, 1, -1]),
    ],
    ids=["defaults", "square", "rectangular-time", "clipped-edge"],
)
def test_chunk_selections_feed_read(tmp_path, pattern, options, indexes):
    data, path, header = store(tmp_path, pattern)
    chunks = rumi.chunks(header, **options)

    for i in indexes:
        selection = chunks[i]
        got = rumi.read(path, header, **selection)
        assert np.array_equal(got, expected(data, selection))


def test_scalar_tiles_make_square_blocks(tmp_path):
    _data, _path, header = store(tmp_path)
    assert list(rumi.chunks(header, tiles=2)) == \
        list(rumi.chunks(header, tiles=(2, 2)))
    assert len(rumi.chunks(header, tiles=2)) == T
    assert rumi.chunks(header, tiles=2)[0]["window"] == (0, 0, 32, 32)


def test_rectangular_blocks_and_band_selection(tmp_path):
    _data, _path, header = store(tmp_path)
    planned = rumi.chunks(header, tiles=(1, 2), bands=[2, 0])
    assert len(planned) == 2 * 1 * T
    assert planned[0]["bands"] == [2, 0]
    assert planned[0]["window"] == (0, 0, 16, 32)

    # A returned selection can be changed without changing the plan.
    planned[0]["bands"].append(1)
    assert planned[0]["bands"] == [2, 0]


def test_clip_keeps_spatial_and_time_remainders(tmp_path):
    data, path, header = store(tmp_path)
    planned = rumi.chunks(header, tiles=2, time=2, edge="clip")

    assert len(planned) == 2 * 2 * 2
    assert planned[-1] == {
        "window": (32, 32, 8, 18),
        "time": (2, 3),
        "bands": None,
    }
    got = rumi.read(path, header, **planned[-1])
    assert np.array_equal(got, data[2:3, :, 32:40, 32:50])


def test_drop_omits_spatial_and_time_remainders(tmp_path):
    _data, _path, header = store(tmp_path)
    planned = rumi.chunks(header, tiles=(1, 2), time=2)
    assert len(planned) == 2
    assert planned[-1]["window"] == (16, 0, 16, 32)
    assert planned[-1]["time"] == (0, 2)


def test_slices_and_parsed_headers(tmp_path):
    _data, _path, header = store(tmp_path)
    planned = rumi.chunks(rumi.RumiHeader(header), bands=(1, 3))
    assert planned[:2] == [planned[0], planned[1]]
    assert planned[-1]["bands"] == (1, 3)


def test_cell_layouts_are_rejected(tmp_path):
    _data, _path, header = store(tmp_path, CELL)
    with pytest.raises(ValueError, match="requires an 'h w' frame layout"):
        rumi.chunks(header)


@pytest.mark.parametrize(
    ("kwargs", "error", "match"),
    [
        ({"tiles": 0}, ValueError, "tiles must be positive"),
        ({"tiles": (1,)}, ValueError, "rows, columns"),
        ({"tiles": (1, 0)}, ValueError, "tile columns must be positive"),
        ({"time": 0}, ValueError, "time must be positive"),
        ({"bands": []}, ValueError, "bands cannot be empty"),
        ({"bands": [B]}, ValueError, "index 3 out"),
        ({"bands": (0, B + 1)}, ValueError, "range .* out"),
        ({"edge": "pad"}, ValueError, "edge must be"),
    ],
)
def test_bad_options_are_rejected(tmp_path, kwargs, error, match):
    _data, _path, header = store(tmp_path)
    with pytest.raises(error, match=match):
        rumi.chunks(header, **kwargs)
