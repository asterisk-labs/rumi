import datetime as dt

import numpy as np
import pytest
import rumi

geozl = pytest.importorskip("geozl")

TRANSFORM = (10.0, 0.0, 300000.0, 0.0, -10.0, 8100000.0)


def stored(tmp_path, name="scene", width=32):
    data = np.arange(2 * 32 * width, dtype=np.uint16).reshape(2, 32, width)
    table = rumi.frames(
        data, "b (row h) (col w) -> row col (b h w)", tile_size=16)
    for frame in table:
        graph = geozl.graph(frame.data, "planar>zigzag>zstd")
        frame.compressed = geozl.compress(frame.data, graph=graph)
    return rumi.write(
        tmp_path / f"{name}.rumi", table,
        time=["2024-08-25"], transform=TRANSFORM, crs=32718)


def test_source_returns_complete_metadata(tmp_path):
    path, header = stored(tmp_path)
    metadata = rumi.info(source=path)

    assert metadata.header == header
    assert metadata.shape == (2, 32, 32)
    assert metadata.time_count == 1
    assert metadata.dtype is np.uint16
    assert metadata.tile == (16, 16)
    assert metadata.frame_layout == "b h w"
    assert metadata.time == [dt.date(2024, 8, 25)]
    assert metadata.time_kind == "instant"
    assert metadata.transform == TRANSFORM
    assert metadata.crs == 32718
    assert metadata.pixel_is_point is False


def test_header_returns_only_metadata_the_header_contains(tmp_path):
    _path, header = stored(tmp_path)
    metadata = rumi.info(header=header)

    assert metadata.header == header
    assert metadata.shape == (2, 32, 32)
    assert metadata.time is None
    assert metadata.time_kind is None
    assert metadata.transform is None
    assert metadata.crs is None
    assert metadata.pixel_is_point is None


def test_source_and_header_validate_their_synchronization(tmp_path):
    path, header = stored(tmp_path)
    assert rumi.info(source=path, header=header).header == header

    _other_path, other = stored(tmp_path, "other", width=48)
    with pytest.raises(ValueError, match="does not match source"):
        rumi.info(source=path, header=other)


def test_memory_uses_the_same_native_indexer(tmp_path):
    path, header = stored(tmp_path)
    assert rumi.info(source=path.read_bytes()).header == header


def test_info_requires_an_input():
    with pytest.raises(ValueError, match="needs source, header, or both"):
        rumi.info()


def test_metadata_repr_does_not_dump_the_binary_header(tmp_path):
    _path, header = stored(tmp_path)
    text = repr(rumi.info(header=header))
    assert text.startswith("<rumi.Metadata (2, 32, 32)>")
    assert "dtype          : uint16" in text
    assert repr(header) not in text


def test_metadata_html_lists_every_attribute(tmp_path):
    path, _header = stored(tmp_path)
    metadata = rumi.info(source=path)
    body = metadata._repr_html_()

    for name in ("shape", "dtype", "tile", "frame_layout", "index_order",
                 "frames", "time", "transform", "crs", "pixel_is_point"):
        assert f">{name}</td>" in body
    assert "<svg" in body
