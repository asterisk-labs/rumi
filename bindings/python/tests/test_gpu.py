import numpy as np
import pytest
import rumi
from _labels import labels
from rumi._ffi import ffi, lib

geozl = pytest.importorskip("geozl")
torch = pytest.importorskip("torch")

pytestmark = pytest.mark.skipif(
    not torch.cuda.is_available(), reason="CUDA is not available")


@pytest.fixture(scope="module")
def scenes(tmp_path_factory):
    rng = np.random.default_rng(42)
    directory = tmp_path_factory.mktemp("gpu")
    out = []
    for i in range(3):
        data = rng.integers(0, 10000, (4, 97, 131), dtype=np.uint16)
        table = rumi.frames(
            data, "b (row h) (col w) -> row col b (h w)", tile_size=32)
        for j, frame in enumerate(table):
            recipe = ("id>entropy" if j % 3 == 0
                      else "planar>zigzag>transpose>pivco")
            graph = geozl.graph(
                frame.data, recipe)
            frame.compressed = geozl.compress(frame.data, graph=graph)
        path, header = rumi.write(
            directory / f"scene-{i}.rumi", table, **labels(table))
        out.append((path, header, data))
    return out


def test_gpu_context_uses_and_pins_the_read_thread_count(scenes):
    configured = rumi.get_num_threads()
    handle = ffi.new("rumi_gpu**")
    rc = lib.rumi_gpu_create(2, 0, 0, 64 * 1024 * 1024, handle)
    assert rc == lib.RUMI_OK
    assert handle[0] != ffi.NULL
    lib.rumi_gpu_destroy(handle[0])

    handle[0] = ffi.NULL
    assert lib.rumi_gpu_create(2, 0, 1025, 1, handle) == lib.RUMI_ERR_INVALID
    assert handle[0] == ffi.NULL

    other = 2 if configured == 1 else 1
    assert lib.rumi_gpu_create(2, 0, other, 1, handle) == lib.RUMI_ERR_STATE
    assert handle[0] == ffi.NULL

    path, header, _ = scenes[0]
    rumi.read(path, header, framework="torch", device="cuda:0",
              window=(0, 0, 32, 32))

    assert rumi.get_num_threads() == configured
    with pytest.raises(RuntimeError, match="thread count is pinned"):
        rumi.set_num_threads(other)


def test_partial_read_writes_a_cuda_tensor(scenes):
    path, header, data = scenes[0]
    with rumi.open(path, header) as dataset:
        result = dataset.read(
            framework="torch", device="cuda:0",
            bands=[3, 1], window=(7, 11, 63, 75))

    assert result.device.type == "cuda"
    expected = torch.from_numpy(data[[3, 1], 7:70, 11:86].copy()).cuda()
    assert torch.equal(result, expected)


def test_chunky_layout_and_nondefault_stream(scenes):
    path, header, data = scenes[0]
    stream = torch.cuda.Stream()
    with torch.cuda.stream(stream):
        result = rumi.read(
            path, header, framework="torch", device="cuda:0",
            pattern="y x b", window=(0, 0, 64, 64))
    stream.synchronize()

    expected = torch.from_numpy(
        np.moveaxis(data[:, :64, :64], 0, -1).copy()).cuda()
    assert torch.equal(result, expected)


def test_read_many_preserves_item_order(scenes):
    windows = [(0, 0, 32, 48), (13, 17, 32, 48), (40, 70, 32, 48)]
    result = rumi.read_many(
        [item[0] for item in scenes],
        [item[1] for item in scenes],
        windows=windows, framework="torch", device="cuda:0")

    expected = np.stack([
        item[2][:, row:row + height, col:col + width]
        for item, (row, col, height, width) in zip(scenes, windows, strict=True)
    ])
    assert torch.equal(result, torch.from_numpy(expected.copy()).cuda())
