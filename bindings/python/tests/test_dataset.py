import os
from pathlib import Path

import numpy as np
import pytest
import rumi
from _labels import labels

geozl = pytest.importorskip("geozl")


@pytest.fixture(scope="module")
def dataset(tmp_path_factory):
    data = np.arange(3 * 48 * 64, dtype=np.uint16).reshape(3, 48, 64)
    table = rumi.frames(
        data, "b (row h) (col w) -> row col b (h w)", tile_size=32)
    for frame in table:
        graph = geozl.graph(frame.data, "planar>zigzag>zstd")
        frame.compressed = geozl.compress(frame.data, graph=graph)
    path, header = rumi.write(
        tmp_path_factory.mktemp("dataset") / "image.rumi",
        table,
        **labels(table),
    )
    return rumi.open(path, header), data


def test_open_carries_the_source_header_and_metadata(dataset):
    file, data = dataset
    assert file.info() is rumi.info(file)
    assert file.info().shape == data.shape
    assert file.header == file.info().header


def test_dataset_read_is_the_same_partial_read(dataset):
    file, data = dataset
    expected = data[[2, 0], 5:21, 7:26]
    assert np.array_equal(
        file.read(bands=[2, 0], window=(5, 7, 16, 19)), expected)
    assert np.array_equal(
        rumi.read(file, bands=[2, 0], window=(5, 7, 16, 19)), expected)


def test_dataset_reuses_its_native_source(dataset, monkeypatch):
    file, _ = dataset

    def unexpected_open(*_args, **_kwargs):
        raise AssertionError("Dataset.read reopened its source")

    monkeypatch.setattr("rumi._read._Source.__init__", unexpected_open)
    assert file.read(window=(0, 0, 1, 1)).shape == (3, 1, 1)


def test_open_creates_one_native_source(dataset, monkeypatch):
    file, _ = dataset
    original = rumi._native._Source.__init__
    calls = 0

    def counted_open(self, target):
        nonlocal calls
        calls += 1
        original(self, target)

    monkeypatch.setattr(rumi._native._Source, "__init__", counted_open)
    with rumi.open(file.source, file.header):
        pass
    assert calls == 1


def test_read_many_accepts_datasets(dataset):
    file, data = dataset
    out = rumi.read_many(
        [file, file],
        windows=[(0, 0, 8, 8), (8, 8, 8, 8)],
        bands=[1],
    )
    assert out.shape == (2, 1, 8, 8)
    assert np.array_equal(out[0], data[[1], :8, :8])
    assert np.array_equal(out[1], data[[1], 8:16, 8:16])


def test_a_dataset_already_owns_its_header(dataset):
    file, _ = dataset
    with pytest.raises(TypeError, match="already carried"):
        rumi.read(file, file.header)


def test_dataset_context_manager_closes_reads(dataset):
    file, _ = dataset
    with rumi.open(file.source, file.header) as opened:
        assert opened.read(window=(0, 0, 1, 1)).shape == (3, 1, 1)
    assert not opened._native_source.handle
    assert not opened._spec.handle
    with pytest.raises(ValueError, match="closed"):
        opened.read(window=(0, 0, 1, 1))


def test_open_expands_home(dataset, monkeypatch):
    file, _ = dataset
    path = Path(file.source)
    home = "USERPROFILE" if os.name == "nt" else "HOME"
    monkeypatch.setenv(home, str(path.parent))
    with rumi.open(f"~/{path.name}", file.header) as opened:
        assert opened.source == str(path)


def test_dataset_repr_is_compact(dataset):
    file, _ = dataset
    text = repr(file)
    assert text.startswith("Dataset(source=")
    assert f"shape={file.metadata.shape}" in text
    assert repr(file.header) not in text


def test_open_owns_mutable_memory(dataset):
    file, data = dataset
    source = bytearray(Path(file.source).read_bytes())
    opened = rumi.open(source, file.header)
    source[:] = b"\x00" * len(source)
    assert np.array_equal(opened.read(), data)
    opened.close()
