"""rumi.geozl writes the frames the GeoZL package writes, with rumi's codec."""

import inspect

import numpy as np
import pytest
import rumi
from _labels import labels

CELL = "b (row h) (col w) -> row col (b h w)"
TILE = "b (row h) (col w) -> row col b (h w)"


def _counts():
    rng = np.random.default_rng(7)
    return rng.integers(200, 4000, (3, 48, 40)).astype(np.uint16)


def _reflectance():
    rng = np.random.default_rng(8)
    return rng.normal(0.3, 0.05, (48, 40)).astype(np.float32)


def _radiance():
    # SQRT fits its noise curve, so the raster needs a spread of intensities.
    y, x = np.mgrid[0:128, 0:128]
    mean = 50 + 4000 * (x + y) / 254
    noise = np.random.default_rng(9).poisson(mean / 50)
    return (mean + noise).astype(np.float32)


def _holes():
    a = _reflectance()
    a[5, 7] = a[20, 3] = np.nan
    return a


CASES = {
    "lossless": (_counts, "planar>zigzag>zstd", {}),
    "linear integer": (_counts, "planar>zigzag>pfor", {"error": 3}),
    "linear float": (_reflectance, "planar>zigzag>zstd", {"error": 0.01}),
    "linear recipe": (_reflectance, "planar>zigzag>zstd",
                      {"error": "LINEAR:MAX_ERROR=0.02"}),
    "log": (_reflectance, "planar>zigzag>zstd", {"error": "1%"}),
    "sqrt": (_radiance, "planar>zigzag>zstd", {"error": "SQRT:MAX_ERROR=0.5N"}),
    "sentinel": (_counts, "planar>zigzag>zstd", {"nodata": 0}),
    "lossy sentinel": (_reflectance, "planar>zigzag>zstd",
                       {"nodata": -9999.0, "error": 0.01}),
    "nan found": (_holes, "planar>zigzag>zstd", {}),
    "nan given": (_holes, "planar>zigzag>zstd", {"nodata": np.nan, "error": 0.01}),
}


@pytest.mark.parametrize("case", CASES)
def test_frames_match_the_geozl_package(case):
    geozl = pytest.importorskip("geozl")
    make, method, options = CASES[case]
    raster = make()
    expected = geozl.compress(raster, graph=geozl.graph(raster, method, **options))
    graph = rumi.geozl.graph(raster, method, **options)
    assert rumi.geozl.compress(raster, graph=graph) == expected


def test_a_lossless_cube_reads_back_exactly(tmp_path):
    data = _counts()
    table = rumi.frames(data, CELL, tile_size=16)
    for frame in table:
        graph = rumi.geozl.graph(frame.data, "planar>zigzag>pfor")
        frame.compressed = rumi.geozl.compress(frame.data, graph=graph)
    path, header = rumi.write(tmp_path / "counts.rumi", table, **labels(table))
    assert np.array_equal(rumi.read(path, header), data)


def test_a_bounded_image_reads_back_within_its_bound(tmp_path):
    data = _reflectance()[None]
    data[0, 0, :4] = -9999.0
    table = rumi.frames(data, TILE, tile_size=16)
    for frame in table:
        graph = rumi.geozl.graph(frame.data, "planar>zigzag>zstd", error=0.01,
                                 nodata=-9999.0)
        frame.compressed = rumi.geozl.compress(frame.data, graph=graph)
    path, header = rumi.write(tmp_path / "reflectance.rumi", table, **labels(table))
    back = rumi.read(path, header)
    hole = data == -9999.0
    assert np.array_equal(back[hole], data[hole])
    assert np.abs(back[~hole] - data[~hole]).max() <= 0.01


def test_a_graph_keeps_its_arguments():
    raster = _counts()
    graph = rumi.geozl.graph(raster, "planar>zigzag>zstd", error=0)
    assert (graph.method, graph.width, graph.planes) == ("planar>zigzag>zstd", 40, 3)
    assert graph.dtype == np.uint16 and graph.error == 0 and graph.nodata is None
    assert rumi.geozl.graph(raster[0], "planar>zigzag>zstd").planes == 1
    with pytest.raises(TypeError, match="cannot be created directly"):
        rumi.geozl.Graph()


def test_the_surface_follows_the_geozl_package():
    graph = inspect.signature(rumi.geozl.graph).parameters
    assert tuple(graph) == ("raster", "method", "width", "planes", "error", "nodata")
    assert tuple(inspect.signature(rumi.geozl.compress).parameters) == ("tile", "graph")


def test_the_core_refuses_what_geozl_would_misread():
    raster = _counts()
    graph = rumi.geozl.graph(raster, "planar>zigzag>zstd")
    with pytest.raises(ValueError, match="the tile dtype is not the graph's"):
        rumi.geozl.compress(raster.astype(np.int16), graph=graph)
    with pytest.raises(ValueError, match="error must not be negative"):
        rumi.geozl.graph(raster, "planar>zigzag>zstd", error=-1)
    with pytest.raises(ValueError, match="between 0% and 100%"):
        rumi.geozl.graph(raster, "planar>zigzag>zstd", error="100%")
    with pytest.raises(ValueError, match="whole rows"):
        rumi.geozl.graph(raster, "planar>zigzag>zstd", planes=7)
    with pytest.raises(ValueError, match="no nodata sentinel"):
        rumi.geozl.graph(raster.astype(np.float16), "planar>zigzag>zstd", nodata=0)
    with pytest.raises(NotImplementedError, match="1, 2, 4 or 8 bytes"):
        rumi.geozl.graph(raster.astype(np.complex128), "planar>zigzag>zstd")
    with pytest.raises(NotImplementedError, match="a bounded error needs"):
        rumi.geozl.graph(raster.astype(np.complex64), "planar>zigzag>zstd", error=1)
    with pytest.raises(ValueError, match="could not build the graph"):
        rumi.geozl.graph(raster, "no such graph")


def test_the_binding_refuses_values_it_cannot_convert():
    raster = _counts()
    with pytest.raises(TypeError, match="not a bool"):
        rumi.geozl.graph(raster, "planar>zigzag>zstd", error=True)
    with pytest.raises(ValueError, match="native byte order"):
        rumi.geozl.graph(raster.astype(">u2"), "planar>zigzag>zstd")
    with pytest.raises(ValueError, match="give width"):
        rumi.geozl.graph(raster.ravel(), "planar>zigzag>zstd")
    with pytest.raises(ValueError, match="whole number"):
        rumi.geozl.graph(raster, "planar>zigzag>zstd", nodata=0.5)
    with pytest.raises(OverflowError, match="does not fit"):
        rumi.geozl.graph(raster, "planar>zigzag>zstd", nodata=-1)
    with pytest.raises(TypeError, match="one sample"):
        rumi.geozl.graph(raster.astype(np.float32), "planar>zigzag>zstd",
                         nodata=[1.0, 2.0])
    with pytest.raises(TypeError, match="must come from rumi.geozl.graph"):
        rumi.geozl.compress(raster, graph=None)
