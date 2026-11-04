import os
import subprocess
import sys
import threading
from concurrent.futures import ThreadPoolExecutor

import numpy as np
import pytest
import rumi
from _labels import labels
from rumi import _gpu
from rumi._ffi import ffi, lib

geozl = pytest.importorskip("geozl")
torch = pytest.importorskip("torch")

_CUDA = lib.RUMI_DEVICE_CUDA
_METAL = lib.RUMI_DEVICE_METAL
_CUDA_AVAILABLE = torch.cuda.is_available() and bool(lib.rumi_gpu_available(_CUDA))
_MPS_AVAILABLE = (torch.backends.mps.is_available()
                  and bool(lib.rumi_gpu_available(_METAL)))
_METAL_VALIDATION = os.environ.get("MTL_DEBUG_LAYER") == "1"
_DEVICES = [
    pytest.param(
        _CUDA, "cuda:0", "cuda",
        marks=pytest.mark.skipif(
            not _CUDA_AVAILABLE, reason="CUDA is not available")),
    pytest.param(
        _METAL, "mps", "mps",
        marks=pytest.mark.skipif(
            not _MPS_AVAILABLE, reason="Metal is not available")),
]
_TORCH_DEVICES = [
    pytest.param(
        "cuda:0", "cuda",
        marks=pytest.mark.skipif(
            not _CUDA_AVAILABLE, reason="CUDA is not available")),
    pytest.param(
        "mps", "mps",
        marks=pytest.mark.skipif(
            not _MPS_AVAILABLE, reason="Metal is not available")),
]


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


@pytest.fixture(scope="module")
def mps_scene(tmp_path_factory):
    data = np.random.default_rng(7).integers(
        0, 3000, (1, 256, 256), dtype=np.uint16)
    table = rumi.frames(
        data, "b (row h) (col w) -> row col b (h w)", tile_size=256)
    for frame in table:
        frame.compressed = geozl.compress(
            frame.data,
            graph=geozl.graph(
                frame.data, "planar>zigzag>transpose>pivco"),
        )
    path, header = rumi.write(
        tmp_path_factory.mktemp("mps") / "scene.rumi",
        table, **labels(table))
    return path, header, data


def _pending_mps_write(image):
    event = torch.mps.event.Event()
    event.record()
    event.synchronize()
    output = torch.nn.functional.max_pool2d(image, 2)
    del output


def test_mps_rejects_a_nonzero_device(scenes):
    path, header, _ = scenes[0]
    with pytest.raises(ValueError, match="MPS exposes only device 0"):
        rumi.read(path, header, framework="torch", device="mps:1")


@pytest.mark.skipif(not _MPS_AVAILABLE, reason="Metal is not available")
@pytest.mark.skipif(
    _METAL_VALIDATION, reason="PyTorch max_pool2d fails under Metal validation")
def test_mps_read_waits_for_pending_torch_work(mps_scene):
    path, header, expected = mps_scene
    image = torch.rand((1, 2, 256, 256), device="mps")
    for _ in range(5):
        _pending_mps_write(image)
        result = rumi.read(
            path, header, framework="torch", device="mps")
        assert np.array_equal(result.cpu().numpy(), expected)


@pytest.mark.skipif(not _MPS_AVAILABLE, reason="Metal is not available")
@pytest.mark.skipif(
    _METAL_VALIDATION, reason="PyTorch max_pool2d fails under Metal validation")
def test_mps_read_many_waits_for_pending_torch_work(mps_scene):
    path, header, data = mps_scene
    expected = np.stack([data, data])
    image = torch.rand((1, 4, 256, 256), device="mps")
    for _ in range(5):
        _pending_mps_write(image)
        result = rumi.read_many(
            [path, path], [header, header],
            framework="torch", device="mps")
        assert np.array_equal(result.cpu().numpy(), expected)


@pytest.mark.skipif(not _MPS_AVAILABLE, reason="Metal is not available")
def test_mps_read_is_safe_while_another_thread_uses_torch(mps_scene):
    path, header, _ = mps_scene
    header_path = path.with_suffix(".header")
    header_path.write_bytes(header)
    src = """
import sys
import threading

import rumi
import torch

path, header_path = sys.argv[1:]
header = open(header_path, "rb").read()
image = torch.rand((1, 2, 256, 256), device="mps")
stop = threading.Event()

def work():
    while not stop.is_set():
        torch.nn.functional.max_pool2d(image, 2).sum().item()

thread = threading.Thread(target=work)
thread.start()
try:
    for _ in range(20):
        rumi.read(path, header, framework="torch", device="mps")
finally:
    stop.set()
    thread.join()
"""
    env = os.environ.copy()
    for name in (
        "MTL_DEBUG_LAYER", "MTL_DEBUG_LAYER_ERROR_MODE",
        "MTL_SHADER_VALIDATION", "MTL_SHADER_VALIDATION_REPORT_TO_STDERR",
    ):
        env.pop(name, None)
    done = subprocess.run(
        [sys.executable, "-c", src, str(path), str(header_path)],
        capture_output=True, text=True, timeout=30, env=env)
    assert done.returncode == 0, done.stderr


def test_mps_requires_versioned_dlpack(monkeypatch):
    monkeypatch.setattr(_gpu, "_wait_for_mps", lambda: None)

    def old_dlpack(_tensor):
        raise TypeError("__dlpack__() got an unexpected keyword argument 'max_version'")

    monkeypatch.setattr(_gpu, "_borrow_versioned", old_dlpack)
    with pytest.raises(
            RuntimeError, match="MPS reads require PyTorch 2.10 or newer"):
        _gpu._metal_target(object())


def test_mps_wait_uses_an_event_not_a_device_synchronize(monkeypatch):
    calls = []

    class Event:
        def record(self):
            calls.append("record")

        def synchronize(self):
            calls.append("synchronize")

    def device_synchronize():
        raise AssertionError("device-wide MPS synchronization is not allowed")

    monkeypatch.setattr(torch.mps.event, "Event", Event)
    monkeypatch.setattr(torch.mps, "synchronize", device_synchronize)

    _gpu._wait_for_mps()

    assert calls == ["record", "synchronize"]


def test_gpu_shutdown_waits_for_an_active_context(monkeypatch):
    started = threading.Event()
    closed = threading.Event()

    class Context:
        lock = threading.Lock()

        @staticmethod
        def close():
            closed.set()

    context = Context()
    monkeypatch.setattr(_gpu, "_contexts", {(_METAL, 0): context})
    monkeypatch.setattr(_gpu, "_closing", False)

    context.lock.acquire()

    def close():
        started.set()
        _gpu._close_contexts()

    thread = threading.Thread(target=close, daemon=True)
    thread.start()
    try:
        assert started.wait(timeout=1)
        assert not closed.is_set()
    finally:
        context.lock.release()
    thread.join(timeout=1)

    assert not thread.is_alive()
    assert closed.is_set()
    assert _gpu._closing


def test_gpu_context_is_not_created_during_shutdown(monkeypatch):
    monkeypatch.setattr(_gpu, "_closing", True)
    with pytest.raises(RuntimeError, match="GPU contexts are shutting down"):
        with _gpu._context(_METAL, 0):
            pass


def test_waiting_for_one_context_does_not_lock_the_context_table(monkeypatch):
    waiting = threading.Event()
    release = threading.Event()

    class Lock:
        def __enter__(self):
            waiting.set()
            release.wait()

        def __exit__(self, *_args):
            pass

    context = _gpu._Context(object(), Lock())
    monkeypatch.setattr(_gpu, "_contexts", {(_CUDA, 0): context})
    monkeypatch.setattr(_gpu, "_closing", False)

    def use_context():
        with _gpu._context(_CUDA, 0):
            pass

    thread = threading.Thread(target=use_context, daemon=True)
    thread.start()
    try:
        assert waiting.wait(timeout=1)
        assert _gpu._contexts_lock.acquire(timeout=1)
        _gpu._contexts_lock.release()
    finally:
        release.set()
    thread.join(timeout=1)
    assert not thread.is_alive()


@pytest.mark.parametrize(("device_type", "device", "torch_type"), _DEVICES)
def test_gpu_context_uses_and_pins_the_read_thread_count(
        scenes, device_type, device, torch_type):
    configured = rumi.get_num_threads()
    handle = ffi.new("rumi_gpu**")
    rc = lib.rumi_gpu_create(
        device_type, 0, 0, 64 * 1024 * 1024, handle)
    assert rc == lib.RUMI_OK
    assert handle[0] != ffi.NULL
    lib.rumi_gpu_destroy(handle[0])

    handle[0] = ffi.NULL
    assert lib.rumi_gpu_create(
        device_type, 0, 1025, 1, handle) == lib.RUMI_ERR_INVALID
    assert handle[0] == ffi.NULL

    other = 2 if configured == 1 else 1
    assert lib.rumi_gpu_create(
        device_type, 0, other, 1, handle) == lib.RUMI_ERR_STATE
    assert handle[0] == ffi.NULL

    path, header, _ = scenes[0]
    result = rumi.read(path, header, framework="torch", device=device,
              window=(0, 0, 32, 32))
    assert result.device.type == torch_type

    assert rumi.get_num_threads() == configured
    with pytest.raises(RuntimeError, match="thread count is pinned"):
        rumi.set_num_threads(other)


@pytest.mark.parametrize(("device", "torch_type"), _TORCH_DEVICES)
def test_partial_read_writes_a_gpu_tensor(
        scenes, device, torch_type):
    path, header, data = scenes[0]
    with rumi.open(path, header) as dataset:
        result = dataset.read(
            framework="torch", device=device,
            bands=[3, 1], window=(7, 11, 63, 75))

    assert result.device.type == torch_type
    expected = data[[3, 1], 7:70, 11:86]
    assert np.array_equal(result.cpu().numpy(), expected)


@pytest.mark.parametrize(("device", "torch_type"), _TORCH_DEVICES)
def test_cpu_decode_backend_places_partial_read_on_gpu(
        scenes, device, torch_type):
    path, header, data = scenes[0]
    result = rumi.read(
        path, header, framework="torch", device=device,
        decode_backend="cpu", bands=[3, 1], window=(7, 11, 63, 75))

    assert result.device.type == torch_type
    assert np.array_equal(result.cpu().numpy(), data[[3, 1], 7:70, 11:86])


@pytest.mark.skipif(not _CUDA_AVAILABLE, reason="CUDA is not available")
@pytest.mark.parametrize("decode_backend", ["auto", "cpu"])
def test_chunky_layout_and_nondefault_stream(scenes, decode_backend):
    path, header, data = scenes[0]
    stream = torch.cuda.Stream()
    with torch.cuda.stream(stream):
        result = rumi.read(
            path, header, framework="torch", device="cuda:0",
            pattern="y x b", window=(0, 0, 64, 64),
            decode_backend=decode_backend)
    stream.synchronize()

    expected = torch.from_numpy(
        np.moveaxis(data[:, :64, :64], 0, -1).copy()).cuda()
    assert torch.equal(result, expected)


@pytest.mark.skipif(not _CUDA_AVAILABLE, reason="CUDA is not available")
def test_cuda_result_can_move_between_streams_with_an_event(scenes):
    path, header, data = scenes[0]
    producer = torch.cuda.Stream(device=0)
    consumer = torch.cuda.Stream(device=0)

    with torch.cuda.stream(producer):
        result = rumi.read(
            path, header, framework="torch", device="cuda:0",
            window=(5, 9, 48, 56))
        ready = producer.record_event()

    consumer.wait_event(ready)
    with torch.cuda.stream(consumer):
        result.record_stream(consumer)
        observed = result.clone()
    consumer.synchronize()

    expected = data[:, 5:53, 9:65]
    assert np.array_equal(observed.cpu().numpy(), expected)


@pytest.mark.skipif(
    not _CUDA_AVAILABLE or torch.cuda.device_count() < 2,
    reason="two CUDA devices are not available")
def test_reads_run_concurrently_on_two_cuda_devices(scenes):
    barrier = threading.Barrier(2)

    def read_on(device_index, scene):
        path, header, data = scene
        with torch.cuda.device(device_index):
            stream = torch.cuda.Stream(device=device_index)
            barrier.wait(timeout=10)
            with torch.cuda.stream(stream):
                result = rumi.read(
                    path, header, framework="torch",
                    device=f"cuda:{device_index}",
                    window=(7, 11, 52, 61))
            stream.synchronize()
            observed = result.cpu().numpy()
        return observed, data[:, 7:59, 11:72]

    with ThreadPoolExecutor(max_workers=2) as pool:
        futures = [
            pool.submit(read_on, 0, scenes[0]),
            pool.submit(read_on, 1, scenes[1]),
        ]
        results = [future.result(timeout=30) for future in futures]

    for observed, expected in results:
        assert np.array_equal(observed, expected)


@pytest.mark.parametrize(("device", "torch_type"), _TORCH_DEVICES)
def test_read_many_preserves_item_order(
        scenes, device, torch_type):
    windows = [(0, 0, 32, 48), (13, 17, 32, 48), (40, 70, 32, 48)]
    result = rumi.read_many(
        [item[0] for item in scenes],
        [item[1] for item in scenes],
        windows=windows, framework="torch", device=device)

    expected = np.stack([
        item[2][:, row:row + height, col:col + width]
        for item, (row, col, height, width) in zip(scenes, windows, strict=True)
    ])
    assert result.device.type == torch_type
    assert np.array_equal(result.cpu().numpy(), expected)


@pytest.mark.parametrize(("device", "torch_type"), _TORCH_DEVICES)
def test_cpu_decode_backend_places_batch_on_gpu(
        scenes, device, torch_type):
    windows = [(0, 0, 32, 48), (13, 17, 32, 48), (40, 70, 32, 48)]
    result = rumi.read_many(
        [item[0] for item in scenes], [item[1] for item in scenes],
        windows=windows, framework="torch", device=device,
        decode_backend="cpu")

    expected = np.stack([
        item[2][:, row:row + height, col:col + width]
        for item, (row, col, height, width) in zip(scenes, windows, strict=True)
    ])
    assert result.device.type == torch_type
    assert np.array_equal(result.cpu().numpy(), expected)


@pytest.mark.skipif(not _MPS_AVAILABLE, reason="Metal is not available")
@pytest.mark.skipif(
    not hasattr(torch.mps, "Stream"), reason="MPS streams are not available")
@pytest.mark.parametrize("decode_backend", ["auto", "cpu"])
def test_mps_read_from_a_nondefault_stream(scenes, decode_backend):
    path, header, data = scenes[0]
    stream = torch.mps.Stream()
    with torch.mps.stream(stream):
        result = rumi.read(
            path, header, framework="torch", device="mps",
            window=(0, 0, 32, 32), decode_backend=decode_backend)

    stream.synchronize()
    assert np.array_equal(result.cpu().numpy(), data[:, :32, :32])
