import datetime as dt
from dataclasses import fields

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
        tmp_path / f"{name}.rumi", table, bands=["red", "nir"],
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
    assert metadata.index_order == ()
    assert metadata.bands == ["red", "nir"]
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
    assert metadata.bands is None
    assert metadata.time is None
    assert metadata.time_kind is None
    assert metadata.transform is None
    assert metadata.crs is None
    assert metadata.pixel_is_point is None


def test_band_preview_is_short_and_escapes_control_characters(tmp_path):
    path, _ = stored(tmp_path)
    metadata = rumi.info(source=path)
    text = "red\n\t\x1b" + "x" * 65500
    metadata.bands = [text]

    preview = repr(metadata)
    assert "red\\n\\t\\x1b" in preview
    assert "x" * 81 not in preview
    assert "x" * 81 not in metadata._repr_html_()
    assert metadata.bands == [text]


def test_source_and_header_validate_their_synchronization(tmp_path):
    path, header = stored(tmp_path)
    assert rumi.info(source=path, header=header).header == header

    _other_path, other = stored(tmp_path, "other", width=48)
    with pytest.raises(ValueError, match="does not match source"):
        rumi.info(source=path, header=other)


def test_memory_uses_the_same_native_indexer(tmp_path):
    path, header = stored(tmp_path)
    assert rumi.info(source=path.read_bytes()).header == header


def test_a_missing_source_is_an_os_error(tmp_path):
    with pytest.raises(OSError, match="could not open"):
        rumi.info(source=tmp_path / "gone.rumi")


def test_a_source_that_ends_early_is_a_format_error(tmp_path):
    path, _header = stored(tmp_path)
    with pytest.raises(ValueError, match="16-byte rumi file header"):
        rumi.info(source=path.read_bytes()[:10])


def test_info_requires_an_input():
    with pytest.raises(ValueError, match="needs source, header, or both"):
        rumi.info()


def test_metadata_repr_does_not_dump_the_binary_header(tmp_path):
    _path, header = stored(tmp_path)
    metadata = rumi.info(header=header)
    text = repr(metadata)
    assert text.startswith("<rumi.Metadata (2, 32, 32)>")
    assert "dtype          : uint16" in text
    pad = max(len(field.name) for field in fields(metadata))
    for field in fields(metadata):
        assert f"\n  {field.name.ljust(pad)} :" in text
    assert repr(header) not in text


def test_metadata_html_lists_every_attribute(tmp_path):
    path, _header = stored(tmp_path)
    metadata = rumi.info(source=path)
    body = metadata._repr_html_()

    for field in fields(metadata):
        assert f">{field.name}</td>" in body
    assert "<svg" in body
    assert "compressed" not in body
    assert repr(metadata.header) not in body
