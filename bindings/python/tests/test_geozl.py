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


def _half():
    # Exercises float16 ties, subnormals and NaNs.
    y, x = np.indices((48, 40))
    a = np.where(y == 0, 2048 + x % 8,
                 np.where(y == 1, (x - 20) / 3 * 2.0 ** -14,
                          ((y * 131 + x * 71) % 1000) / 7 - 60)).astype(np.float16)
    a[(y * 40 + x) % 89 == 0] = np.nan
    return a


def _wide():
    a = np.random.default_rng(10).integers(-2**62, 2**62, (48, 40), dtype=np.int64)
    a[0, :3] = [2**63 - 1, -2**63 + 1, -2**63]
    a[5::7, 3::5] = -2**63
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
    "half": (_half, "planar>zigzag>zstd", {}),
    "int64 sentinel": (_wide, "planar>zigzag>zstd", {"nodata": -2**63}),
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
    assert graph.dtype == np.uint16 and graph.error is None and graph.nodata is None
    assert rumi.geozl.graph(raster[0], "planar>zigzag>zstd").planes == 1
    with pytest.raises(TypeError, match="cannot be created directly"):
        rumi.geozl.Graph()


@pytest.mark.parametrize("error", [None, 0, "0%", 3, 0.01, "1%", "LINEAR:MAX_ERROR=0.02"])
def test_a_graph_reports_the_recipe_geozl_reports(error):
    geozl = pytest.importorskip("geozl")
    raster = _reflectance()
    expected = geozl.graph(raster, "planar>zigzag>zstd", error=error).error
    assert rumi.geozl.graph(raster, "planar>zigzag>zstd", error=error).error == expected


def test_geozl_is_an_importable_submodule():
    import rumi.geozl as module
    from rumi.geozl import Graph, compress, graph

    assert module is rumi.geozl
    assert (Graph, compress, graph) == (module.Graph, module.compress, module.graph)
    assert module.__all__ == ["Graph", "compress", "graph"]


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
    with pytest.raises(OverflowError, match="does not fit"):
        rumi.geozl.graph(raster.astype(np.int64), "planar>zigzag>zstd", nodata=2**63)
    with pytest.raises(FloatingPointError):
        rumi.geozl.graph(raster.astype(np.float32), "planar>zigzag>zstd", nodata=1e39)
    with pytest.raises(TypeError, match="one sample"):
        rumi.geozl.graph(raster.astype(np.float32), "planar>zigzag>zstd",
                         nodata=[1.0, 2.0])
    with pytest.raises(TypeError, match="must come from rumi.geozl.graph"):
        rumi.geozl.compress(raster, graph=None)
    with pytest.raises(ValueError, match="NUL character"):
        rumi.geozl.graph(raster, "planar>zigzag>zstd\x00x")
    with pytest.raises(ValueError, match="NUL character"):
        rumi.geozl.graph(raster, "planar>zigzag>zstd", error="1%\x00x")
    for text in ("5", "", "0"):
        with pytest.raises(ValueError, match="a text error is a percentage"):
            rumi.geozl.graph(raster, "planar>zigzag>zstd", error=text)


def test_a_bool_tile_holds_only_zero_and_one():
    flags = np.frombuffer(bytes([0, 1, 1, 0] * 4), dtype=bool).reshape(4, 4)
    graph = rumi.geozl.graph(flags, "planar>zigzag>zstd")
    assert rumi.geozl.compress(flags, graph=graph)
    twos = np.frombuffer(bytes([0, 1, 2, 0] * 4), dtype=bool).reshape(4, 4)
    with pytest.raises(ValueError, match="other than 0 or 1"):
        rumi.geozl.compress(twos, graph=graph)
