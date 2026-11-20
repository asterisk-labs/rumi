"""rumi.geozl writes the frames the GeoZL package writes, with rumi's codec."""

import copy
import inspect
import pickle
from concurrent.futures import ThreadPoolExecutor
from decimal import Decimal
from fractions import Fraction

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


def _current_geozl():
    """The geozl package, when it is recent enough to share Rumi's argument checks."""
    geozl = pytest.importorskip("geozl")
    if not hasattr(geozl, "GeoZLError"):
        pytest.skip("geozl predates the shared argument checks; the submodule moves "
                    "with the next geozl release")
    return geozl


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
    assert rumi.geozl.compress(raster, graph) == expected


def test_a_lossless_cube_reads_back_exactly(tmp_path):
    data = _counts()
    table = rumi.frames(data, CELL, tile_size=16)
    for frame in table:
        graph = rumi.geozl.graph(frame.data, "planar>zigzag>pfor")
        frame.compressed = rumi.geozl.compress(frame.data, graph=graph)
    path = tmp_path / "counts.rumi"
    header = rumi.write(path, table, **labels(table))
    assert np.array_equal(rumi.read(path, header), data)


def test_a_bounded_image_reads_back_within_its_bound(tmp_path):
    data = _reflectance()[None]
    data[0, 0, :4] = -9999.0
    table = rumi.frames(data, TILE, tile_size=16)
    for frame in table:
        graph = rumi.geozl.graph(frame.data, "planar>zigzag>zstd", error=0.01,
                                 nodata=-9999.0)
        frame.compressed = rumi.geozl.compress(frame.data, graph=graph)
    path = tmp_path / "reflectance.rumi"
    header = rumi.write(path, table, **labels(table))
    back = rumi.read(path, header)
    hole = data == -9999.0
    assert np.array_equal(back[hole], data[hole])
    assert np.abs(back[~hole] - data[~hole]).max() <= 0.01


def test_a_graph_keeps_its_arguments():
    raster = _counts()
    graph = rumi.geozl.graph(raster, "planar>zigzag>zstd", error=0)
    assert (graph.method, graph.width, graph.planes) == ("planar>zigzag>zstd", 40, 3)
    assert graph.dtype == np.uint16 and graph.error is None and graph.nodata is None
    assert graph.itemsize == 2
    assert repr(graph) == ("<rumi.geozl.Graph planar>zigzag>zstd, uint16, row width 40, "
                           "3 planes, lossless>")
    one = rumi.geozl.graph(raster[0], "planar>zigzag>pfor", error=3)
    assert one.planes == 1
    assert repr(one) == ("<rumi.geozl.Graph planar>zigzag>pfor, uint16, row width 40, "
                         "1 plane, LINEAR:MAX_ERROR=3>")
    with pytest.raises(TypeError, match="cannot be created directly"):
        rumi.geozl.Graph()


def test_threads_that_share_a_graph_take_turns():
    raster = _counts()
    graph = rumi.geozl.graph(raster, "planar>zigzag>zstd")
    want = rumi.geozl.compress(raster, graph)
    with ThreadPoolExecutor(4) as pool:
        frames = list(pool.map(lambda _: rumi.geozl.compress(raster, graph), range(64)))
    assert frames == [want] * 64


def test_a_graph_cannot_be_pickled_or_copied():
    graph = rumi.geozl.graph(_counts(), "planar>zigzag>zstd")
    for clone in (pickle.dumps, copy.copy, copy.deepcopy):
        with pytest.raises(TypeError, match="cannot be pickled or copied"):
            clone(graph)


@pytest.mark.parametrize("error", [None, 0, "0%", 3, 0.01, "1%", "LINEAR:MAX_ERROR=0.02"])
def test_a_graph_reports_the_recipe_geozl_reports(error):
    geozl = pytest.importorskip("geozl")
    raster = _reflectance()
    theirs = geozl.graph(raster, "planar>zigzag>zstd", error=error)
    ours = rumi.geozl.graph(raster, "planar>zigzag>zstd", error=error)
    assert ours.error == theirs.error


@pytest.mark.parametrize("error", [None, 3, "1%"])
def test_a_graph_prints_as_geozl_prints_it(error):
    geozl = _current_geozl()
    raster = _reflectance()
    theirs = geozl.graph(raster, "planar>zigzag>zstd", error=error)
    ours = rumi.geozl.graph(raster, "planar>zigzag>zstd", error=error)
    assert ours.itemsize == theirs.itemsize
    assert repr(ours) == repr(theirs).replace("<geozl.", "<rumi.geozl.")


def test_geozl_is_an_importable_submodule():
    import rumi.geozl as module
    from rumi.geozl import Graph, compress, graph

    assert module is rumi.geozl
    assert (Graph, compress, graph) == (module.Graph, module.compress, module.graph)
    assert module.__all__ == ["Graph", "compress", "graph"]


def test_the_surface_follows_the_geozl_package():
    graph = inspect.signature(rumi.geozl.graph).parameters
    assert tuple(graph) == ("raster", "method", "width", "planes", "error", "nodata")
    compress = inspect.signature(rumi.geozl.compress).parameters
    assert tuple(compress) == ("tile", "graph")
    # graph may also be passed by position, as in R and Julia.
    assert compress["graph"].kind is inspect.Parameter.POSITIONAL_OR_KEYWORD


def test_the_core_refuses_what_geozl_would_misread():
    raster = _counts()
    graph = rumi.geozl.graph(raster, "planar>zigzag>zstd")
    with pytest.raises(ValueError, match="^this graph was built for dtype uint16, got int16$"):
        rumi.geozl.compress(raster.astype(np.int16), graph=graph)
    with pytest.raises(ValueError, match="^error must not be negative, got -1$"):
        rumi.geozl.graph(raster, "planar>zigzag>zstd", error=-1)
    with pytest.raises(ValueError, match='^relative error must be between 0% and 100%, '
                                         'got "100%"$'):
        rumi.geozl.graph(raster, "planar>zigzag>zstd", error="100%")
    with pytest.raises(ValueError, match="^7 planes do not split 5760 elements into whole "
                                         "rows of 40$"):
        rumi.geozl.graph(raster, "planar>zigzag>zstd", planes=7)
    with pytest.raises(ValueError, match="^a half float carries no sentinel"):
        rumi.geozl.graph(raster.astype(np.float16), "planar>zigzag>zstd", nodata=0)
    # A 16-byte sample is an invalid request now, not an unsupported one.
    with pytest.raises(ValueError, match="^dtype complex128 is 16 bytes per element"):
        rumi.geozl.graph(raster.astype(np.complex128), "planar>zigzag>zstd")
    with pytest.raises(ValueError, match="^the quantizers do not support dtype complex64$"):
        rumi.geozl.graph(raster.astype(np.complex64), "planar>zigzag>zstd", error=1)
    with pytest.raises(ValueError, match="^nodata needs a dtype geozl knows, got bool$"):
        rumi.geozl.graph(raster.astype(bool), "planar>zigzag>zstd", nodata=0)
    with pytest.raises(ValueError, match='^GeoZL graph failed \\(method = "no such graph"\\): '
                                         '.* \\(ZL error code 21\\)$') as raised:
        rumi.geozl.graph(raster, "no such graph")
    assert not isinstance(raised.value, rumi.RumiError)


def test_a_bad_recipe_is_a_value_error_in_both_packages():
    geozl = _current_geozl()
    for module in (geozl, rumi.geozl):
        with pytest.raises(ValueError, match="no such graph"):
            module.graph(_counts(), "no such graph")


M = "planar>zigzag>zstd"

# Bad arguments both packages check, each as a call on the module under test.
SHARED_REFUSALS = {
    "method not text": lambda m: m.graph(_counts(), 7),
    "empty method": lambda m: m.graph(_counts(), ""),
    "NUL in method": lambda m: m.graph(_counts(), M + "\x00x"),
    "bool error": lambda m: m.graph(_counts(), M, error=True),
    "NumPy bool error": lambda m: m.graph(_counts(), M, error=np.bool_(False)),
    "error of another type": lambda m: m.graph(_counts(), M, error=[1]),
    "NUL in error": lambda m: m.graph(_counts(), M, error="1%\x00x"),
    "bare number text": lambda m: m.graph(_counts(), M, error="5"),
    "empty error text": lambda m: m.graph(_counts(), M, error=""),
    "float width": lambda m: m.graph(_counts(), M, width=3.7),
    "text width": lambda m: m.graph(_counts(), M, width="40"),
    "bool width": lambda m: m.graph(_counts(), M, width=True),
    "zero width": lambda m: m.graph(_counts(), M, width=0),
    "width past uint32": lambda m: m.graph(_counts(), M, width=2**32),
    "zero planes": lambda m: m.graph(_counts(), M, planes=0),
    "float planes": lambda m: m.graph(_counts(), M, planes=1.0),
    "1-D without width": lambda m: m.graph(_counts().ravel(), M),
    "text nodata": lambda m: m.graph(_counts(), M, nodata="0"),
    "bool nodata": lambda m: m.graph(_counts(), M, nodata=True),
    "nodata list": lambda m: m.graph(_reflectance(), M, nodata=[1.0, 2.0]),
    "NaN nodata on integers": lambda m: m.graph(_counts(), M, nodata=np.nan),
    "fractional nodata": lambda m: m.graph(_counts(), M, nodata=0.5),
    "negative nodata on unsigned": lambda m: m.graph(_counts(), M, nodata=-1),
    "nodata past int64": lambda m: m.graph(_counts().astype(np.int64), M, nodata=2**63),
    "nodata past float32": lambda m: m.graph(_reflectance(), M, nodata=1e39),
    "fractional Fraction nodata": lambda m: m.graph(_counts(), M, nodata=Fraction(3, 2)),
    "fractional Decimal nodata": lambda m: m.graph(_counts(), M, nodata=Decimal("2.5")),
    "Decimal NaN on integers": lambda m: m.graph(_counts(), M, nodata=Decimal("NaN")),
    "signaling Decimal NaN on integers": lambda m: m.graph(_counts(), M,
                                                           nodata=Decimal("sNaN")),
    "Decimal past uint16": lambda m: m.graph(_counts(), M, nodata=Decimal("65536")),
    "Decimal past float32": lambda m: m.graph(_reflectance(), M, nodata=Decimal("1e400")),
    "swapped bytes": lambda m: m.graph(_counts().astype(">u2"), M),
    "object samples": lambda m: m.graph(np.array([[1, 2]], dtype=object), M),
    "compress without a graph": lambda m: m.compress(_counts(), graph=None),
    "compress with a recipe": lambda m: m.compress(_counts(), graph=M),
}


# Bad arguments Rumi's core checks with geozl's text. The core quotes a text
# value in double quotes where geozl prints its repr, so these compare after
# turning single quotes into double ones.
CORE_REFUSALS = {
    "negative integer error": lambda m: m.graph(_counts(), M, error=-1),
    "negative float error": lambda m: m.graph(_counts(), M, error=-0.5),
    "infinite error": lambda m: m.graph(_counts(), M, error=float("inf")),
    "NaN error": lambda m: m.graph(_counts(), M, error=float("nan")),
    "percentage past 100": lambda m: m.graph(_counts(), M, error="150%"),
    "negative percentage": lambda m: m.graph(_counts(), M, error="-5%"),
    "percentage of text": lambda m: m.graph(_counts(), M, error="ten%"),
    "planes that split rows": lambda m: m.graph(_counts()[0, :16, :16], M, planes=3),
    "half float sentinel": lambda m: m.graph(_half(), M, nodata=0),
    "16-byte samples": lambda m: m.graph(_counts().astype(np.complex128), M),
    "bound on complex": lambda m: m.graph(_counts().astype(np.complex64), M, error=1),
    "sentinel on bool": lambda m: m.graph(_counts().astype(bool), M, nodata=0),
    "tile of another dtype": lambda m: m.compress(
        _counts().astype(np.int16), graph=m.graph(_counts(), M)),
}


@pytest.mark.parametrize("case", SHARED_REFUSALS)
def test_a_shared_check_refuses_as_geozl_does(case):
    geozl = _current_geozl()
    call = SHARED_REFUSALS[case]
    with pytest.raises(Exception) as theirs:
        call(geozl)
    with pytest.raises(Exception) as ours:
        call(rumi.geozl)
    assert type(ours.value) is type(theirs.value)
    assert str(ours.value) == str(theirs.value).replace("geozl.", "rumi.geozl.")


@pytest.mark.parametrize("case", CORE_REFUSALS)
def test_a_core_check_refuses_as_geozl_does(case):
    geozl = pytest.importorskip("geozl")
    call = CORE_REFUSALS[case]
    with pytest.raises(Exception) as theirs:
        call(geozl)
    with pytest.raises(Exception) as ours:
        call(rumi.geozl)
    assert type(ours.value) is type(theirs.value)
    assert str(ours.value) == str(theirs.value).replace("'", '"')


@pytest.mark.parametrize("make, nodata", [
    (_counts, Decimal("200")), (_counts, Fraction(200, 1)),
    (_holes, Decimal("sNaN")), (_holes, Decimal("-9999.5"))])
def test_an_exact_sentinel_writes_the_frame_geozl_writes(make, nodata):
    geozl = _current_geozl()
    raster = make()
    expected = geozl.compress(raster, graph=geozl.graph(raster, M, nodata=nodata))
    assert rumi.geozl.compress(raster, rumi.geozl.graph(raster, M, nodata=nodata)) == expected


def test_an_exact_sentinel_is_checked_without_rounding():
    raster = _counts()
    frame = rumi.geozl.compress(raster, rumi.geozl.graph(raster, M, nodata=200))
    for exact in (Decimal("200"), Fraction(200, 1), Decimal("2E+2")):
        assert rumi.geozl.compress(raster, rumi.geozl.graph(raster, M, nodata=exact)) == frame
    with pytest.raises(ValueError, match="^nodata 3/2 is not a whole number"):
        rumi.geozl.graph(raster, M, nodata=Fraction(3, 2))
    with pytest.raises(ValueError, match="^a uint16 raster cannot use NaN as nodata$"):
        rumi.geozl.graph(raster, M, nodata=Decimal("sNaN"))
    with pytest.raises(ValueError, match="^nodata does not fit the dtype$"):
        rumi.geozl.graph(_reflectance(), M, nodata=Decimal("1e400"))
    # A signaling Decimal NaN on a float raster marks NaN holes, as a quiet one does.
    holes = _holes()
    nan = rumi.geozl.compress(holes, rumi.geozl.graph(holes, M, nodata=np.nan))
    assert rumi.geozl.compress(holes, rumi.geozl.graph(holes, M, nodata=Decimal("sNaN"))) == nan


def test_the_binding_refuses_values_it_cannot_convert():
    raster = _counts()
    with pytest.raises(ValueError, match="^error must not be a boolean$"):
        rumi.geozl.graph(raster, M, error=True)
    with pytest.raises(ValueError, match="not native byte order"):
        rumi.geozl.graph(raster.astype(">u2"), M)
    with pytest.raises(TypeError, match="^width must be a whole number$"):
        rumi.geozl.graph(raster, M, width=40.0)
    with pytest.raises(ValueError, match="^planes must be from 1 to 4294967295$"):
        rumi.geozl.graph(raster, M, planes=0)
    with pytest.raises(TypeError, match="^nodata must be a number$"):
        rumi.geozl.graph(raster, M, nodata="0")
    with pytest.raises(ValueError, match="^a uint16 raster cannot use NaN as nodata$"):
        rumi.geozl.graph(raster, M, nodata=np.nan)
    with pytest.raises(ValueError, match="^nodata 0.5 is not a whole number"):
        rumi.geozl.graph(raster, M, nodata=0.5)
    with pytest.raises(ValueError, match="^nodata does not fit the dtype$"):
        rumi.geozl.graph(raster.astype(np.float32), M, nodata=1e39)
    with pytest.raises(TypeError, match="^graph must be a rumi.geozl.Graph, got NoneType$"):
        rumi.geozl.compress(raster, None)
    with pytest.raises(ValueError, match="^method must not contain a NUL character$"):
        rumi.geozl.graph(raster, M + "\x00x")
    for text in ("5", "", "0"):
        with pytest.raises(ValueError, match="a percentage such as"):
            rumi.geozl.graph(raster, M, error=text)


def test_a_bool_tile_holds_only_zero_and_one():
    flags = np.frombuffer(bytes([0, 1, 1, 0] * 4), dtype=bool).reshape(4, 4)
    graph = rumi.geozl.graph(flags, "planar>zigzag>zstd")
    assert rumi.geozl.compress(flags, graph=graph)
    twos = np.frombuffer(bytes([0, 1, 2, 0] * 4), dtype=bool).reshape(4, 4)
    with pytest.raises(ValueError, match="other than 0 or 1"):
        rumi.geozl.compress(twos, graph=graph)
