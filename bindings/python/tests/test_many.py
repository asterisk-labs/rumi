"""Multi-source window reads."""

import re
import threading
from concurrent.futures import ThreadPoolExecutor
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import numpy as np
import pytest
import rumi

geozl = pytest.importorskip("geozl")

GRAPH = "planar>zigzag>zstd"
PATTERN = "b (row h) (col w) -> row col b (h w)"
TILE = 32


def _write(path, data, pattern=PATTERN):
    tf = rumi.frames(data, pattern, TILE)
    graphs = {}
    for frame in tf:
        graph = graphs.get(frame.data.shape)
        if graph is None:
            graph = graphs[frame.data.shape] = geozl.graph(frame.data, GRAPH)
        frame.compressed = geozl.compress(frame.data, graph=graph)
    return rumi.write(path, tf)


@pytest.fixture(scope="module")
def scenes(tmp_path_factory):
    """Four square scenes and one wider scene."""
    rng = np.random.default_rng(0)
    directory = tmp_path_factory.mktemp("batch")
    out = []
    for i in range(4):
        data = rng.integers(0, 3000, (3, 96, 96)).astype(np.uint16)
        path, header = _write(directory / f"scene{i}.rumi", data)
        out.append((str(path), header, data))
    wide = rng.integers(0, 3000, (3, 96, 160)).astype(np.uint16)
    path, header = _write(directory / "wide.rumi", wide)
    out.append((str(path), header, wide))
    return out


@pytest.fixture
def square(scenes):
    return scenes[:4]


class TestAgreement:
    def test_a_batch_matches_the_reads_it_replaces(self, square):
        windows = [(0, 0, 32, 32), (32, 0, 32, 32), (0, 32, 32, 32),
                   (64, 64, 32, 32)]
        paths = [p for p, _h, _d in square]
        headers = [h for _p, h, _d in square]

        batch = rumi.read_many(paths, headers, windows=windows)
        assert batch.shape == (4, 3, 32, 32)
        for i, window in enumerate(windows):
            assert np.array_equal(
                batch[i], rumi.read(paths[i], headers[i], window=window)
            )

    def test_one_shared_window_matches_individual_reads(self, square):
        paths = [p for p, _h, _d in square]
        headers = [h for _p, h, _d in square]
        window = (0, 0, 64, 64)
        batch = rumi.read_many(paths, headers, windows=[window] * 4)
        for i, (path, header) in enumerate(zip(paths, headers, strict=True)):
            assert np.array_equal(
                batch[i], rumi.read(path, header, window=window))

    def test_the_same_source_may_appear_more_than_once(self, square):
        path, header, data = square[0]
        batch = rumi.read_many([path, path], [header, header],
                                windows=[(0, 0, 32, 32), (32, 32, 32, 32)])
        assert np.array_equal(batch[0], data[:, 0:32, 0:32])
        assert np.array_equal(batch[1], data[:, 32:64, 32:64])

    def test_one_item_keeps_the_batch_axis(self, square):
        path, header, data = square[0]
        batch = rumi.read_many([path], [header],
                                windows=[(0, 0, 32, 32)])
        assert batch.shape == (1, 3, 32, 32)
        assert np.array_equal(batch[0], data[:, :32, :32])

    def test_items_keep_the_order_they_were_given(self, square):
        paths = [p for p, _h, _d in square]
        headers = [h for _p, h, _d in square]
        window = (0, 0, 32, 32)
        forward = rumi.read_many(paths, headers, windows=[window] * 4)
        reverse = rumi.read_many(paths[::-1], headers[::-1],
                                  windows=[window] * 4)
        assert np.array_equal(forward, reverse[::-1])

    def test_compatible_frame_orders_share_a_batch(self, tmp_path):
        data = np.arange(3 * 2 * 48 * 48, dtype=np.uint16).reshape(3, 2, 48, 48)
        planar = "t b (row h) (col w) -> row col (b t h w)"
        chunky = "t b (row h) (col w) -> row col (h w t b)"
        path_a, header_a = _write(tmp_path / "planar.rumi", data, planar)
        path_b, header_b = _write(tmp_path / "chunky.rumi", data + 10000, chunky)
        windows = [(3, 5, 20, 18), (11, 7, 20, 18)]

        batch = rumi.read_many(
            [path_a, path_b], [header_a, header_b], windows=windows,
            time=[2, 0], bands=[1, 0],
        )
        expected = np.stack([
            data[[2, 0]][:, [1, 0], 3:23, 5:23],
            (data + 10000)[[2, 0]][:, [1, 0], 11:31, 7:25],
        ])
        assert np.array_equal(batch, expected)


class TestSources:
    def test_successive_reads_reuse_the_connection(self, square):
        path, header, data = square[0]
        payload = Path(path).read_bytes()
        state = {"connections": 0, "gets": 0}

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def setup(self):
                super().setup()
                state["connections"] += 1

            def do_GET(self):
                match = re.fullmatch(r"bytes=(\d+)-(\d+)",
                                     self.headers.get("Range", ""))
                if match is None:
                    self.send_error(400)
                    return
                first, last = map(int, match.groups())
                body = payload[first:last + 1]
                state["gets"] += 1
                self.send_response(206)
                self.send_header("Content-Range",
                                 f"bytes {first}-{last}/{len(payload)}")
                self.send_header("Content-Length", str(len(body)))
                if state["gets"] == 2:
                    self.send_header("Connection", "close")
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, _format, *_args):
                pass

        server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        thread = threading.Thread(target=server.serve_forever)
        thread.start()
        try:
            url = f"http://127.0.0.1:{server.server_port}/scene.rumi"
            window = (0, 0, 32, 32)
            first = rumi.read(url, header, window=window)
            second = rumi.read(url, header, window=window)
        finally:
            server.shutdown()
            server.server_close()
            thread.join()

        assert np.array_equal(first, data[:, :32, :32])
        assert np.array_equal(second, first)
        assert state == {"connections": 1, "gets": 2}

    def test_remote_uris_use_the_internal_transport(self, square, monkeypatch):
        paths = [item[0] for item in square[:2]]
        headers = [item[1] for item in square[:2]]
        data = [item[2] for item in square[:2]]
        objects = {
            f"/scene{i}.rumi": Path(path).read_bytes()
            for i, path in enumerate(paths)
        }
        state = {"gets": 0, "heads": 0, "operation_headers": []}

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def do_HEAD(self):
                state["heads"] += 1
                self.send_error(405)

            def do_GET(self):
                state["gets"] += 1
                state["operation_headers"].append(
                    self.headers.get("X-Rumi-Operation"))
                payload = objects.get(self.path)
                match = re.fullmatch(r"bytes=(\d+)-(\d+)",
                                     self.headers.get("Range", ""))
                if payload is None or match is None:
                    self.send_error(400)
                    return
                first, last = map(int, match.groups())
                if first >= len(payload) or last < first:
                    self.send_error(416)
                    return
                last = min(last, len(payload) - 1)
                body = payload[first:last + 1]
                self.send_response(206)
                self.send_header("Content-Range",
                                 f"bytes {first}-{last}/{len(payload)}")
                self.send_header("Content-Length", str(len(body)))
                self.send_header("Connection", "close")
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, _format, *_args):
                pass

        server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        thread = threading.Thread(target=server.serve_forever)
        thread.start()
        try:
            base = f"http://127.0.0.1:{server.server_port}"
            urls = [f"{base}/scene{i}.rumi" for i in range(2)]
            windows = [(0, 0, 32, 32), (32, 32, 32, 32)]
            monkeypatch.setenv("GDAL_HTTP_HEADERS", "X-Rumi-Operation: batch")
            got = rumi.read_many(urls, headers, windows=windows)
            monkeypatch.setenv("GDAL_HTTP_HEADERS", "X-Rumi-Operation: single")
            one = rumi.read(urls[0], headers[0], window=windows[0])
            with pytest.raises(TypeError, match="'headers'"):
                rumi.read_many(urls, windows=windows)
        finally:
            server.shutdown()
            server.server_close()
            thread.join()

        assert np.array_equal(got[0], data[0][:, :32, :32])
        assert np.array_equal(got[1], data[1][:, 32:64, 32:64])
        assert np.array_equal(one, data[0][:, :32, :32])
        assert state["gets"] == 3
        assert state["heads"] == 0
        assert state["operation_headers"].count("batch") == 2
        assert state["operation_headers"].count("single") == 1

    def test_remote_errors_keep_the_transport_detail(self, square):
        _path, header, _data = square[0]
        body = b"x" * 170 + b"transport-detail" + b"y" * 20

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def do_GET(self):
                self.send_response(403)
                self.send_header("Content-Type", "text/plain")
                self.send_header("Content-Length", str(len(body)))
                self.send_header("Connection", "close")
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, _format, *_args):
                pass

        server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        thread = threading.Thread(target=server.serve_forever)
        thread.start()
        try:
            url = f"http://127.0.0.1:{server.server_port}/unavailable.rumi"
            with pytest.raises(IOError, match="transport-detail"):
                rumi.read(url, header, window=(0, 0, 32, 32))
        finally:
            server.shutdown()
            server.server_close()
            thread.join()

    def test_scenes_of_different_extents_share_a_batch(self, scenes):
        small_path, small_header, small_data = scenes[0]
        wide_path, wide_header, wide_data = scenes[4]
        batch = rumi.read_many(
            [small_path, wide_path], [small_header, wide_header],
            windows=[(0, 0, 32, 32), (0, 128, 32, 32)],
        )
        assert np.array_equal(batch[0], small_data[:, 0:32, 0:32])
        assert np.array_equal(batch[1], wide_data[:, 0:32, 128:160])

    def test_info_rebuilds_the_headers_for_existing_paths(self, square):
        paths = [p for p, _h, _d in square]
        headers = [h for _p, h, _d in square]
        rebuilt = [rumi.info(source=path).header for path in paths]
        windows = [(0, 0, 32, 32)] * 4
        assert rebuilt == headers
        assert np.array_equal(
            rumi.read_many(paths, rebuilt, windows=windows),
            rumi.read_many(paths, headers, windows=windows),
        )

    def test_bytes_sources_read_the_same_samples(self, square):
        paths = [p for p, _h, _d in square]
        headers = [h for _p, h, _d in square]
        blobs = [open(p, "rb").read() for p in paths]
        windows = [(0, 0, 32, 32), (32, 32, 32, 32), (0, 0, 32, 32),
                   (64, 0, 32, 32)]
        assert np.array_equal(
            rumi.read_many(blobs, headers, windows=windows),
            rumi.read_many(paths, headers, windows=windows),
        )

    def test_a_scene_smaller_than_one_tile_batches_with_a_full_one(
            self, tmp_path, square):
        rng = np.random.default_rng(2)
        tiny = rng.integers(0, 3000, (3, 20, 20)).astype(np.uint16)
        tiny_path, tiny_header = _write(tmp_path / "tiny.rumi", tiny)
        big_path, big_header, big_data = square[0]

        window = (0, 0, 20, 20)
        batch = rumi.read_many([str(tiny_path), big_path],
                                [tiny_header, big_header],
                                windows=[window, window])
        assert np.array_equal(batch[0], tiny[:, 0:20, 0:20])
        assert np.array_equal(batch[1], big_data[:, 0:20, 0:20])

        flipped = rumi.read_many([big_path, str(tiny_path)],
                                  [big_header, tiny_header],
                                  windows=[window, window])
        assert np.array_equal(flipped[0], big_data[:, 0:20, 0:20])
        assert np.array_equal(flipped[1], tiny[:, 0:20, 0:20])


class TestSelection:
    def test_bands_apply_to_every_item(self, square):
        paths = [p for p, _h, _d in square]
        headers = [h for _p, h, _d in square]
        windows = [(0, 0, 32, 32), (32, 32, 32, 32)]
        batch = rumi.read_many(paths[:2], headers[:2], windows=windows,
                                bands=[0, 2])
        assert batch.shape == (2, 2, 32, 32)
        for i, window in enumerate(windows):
            assert np.array_equal(
                batch[i],
                rumi.read(paths[i], headers[i], window=window, bands=[0, 2]),
            )

    def test_a_pattern_reorders_the_output(self, square):
        paths = [p for p, _h, _d in square]
        headers = [h for _p, h, _d in square]
        windows = [(0, 0, 32, 32)] * 2
        default = rumi.read_many(paths[:2], headers[:2], windows=windows)
        moved = rumi.read_many(paths[:2], headers[:2], windows=windows,
                                pattern="n y x b")
        assert moved.shape == (2, 32, 32, 3)
        assert np.array_equal(moved, np.moveaxis(default, 1, -1))

class TestFrameworks:
    def test_torch_receives_the_same_samples(self, square):
        torch = pytest.importorskip("torch")
        paths = [p for p, _h, _d in square]
        headers = [h for _p, h, _d in square]
        windows = [(0, 0, 32, 32), (32, 32, 32, 32)]
        tensor = rumi.read_many(paths[:2], headers[:2], windows=windows,
                                 framework="torch")
        assert isinstance(tensor, torch.Tensor)
        assert np.array_equal(
            tensor.numpy(),
            rumi.read_many(paths[:2], headers[:2], windows=windows),
        )

    def test_dataloader_uses_one_prebatched_call(self, square):
        torch = pytest.importorskip("torch")
        paths = [p for p, _h, _d in square]
        headers = [h for _p, h, _d in square]
        windows = [(0, 0, 32, 32), (32, 0, 32, 32),
                   (0, 32, 32, 32), (64, 64, 32, 32)]
        calls = []

        class Dataset(torch.utils.data.Dataset):
            def __len__(self):
                return len(paths)

            def __getitem__(self, _index):
                raise AssertionError("DataLoader bypassed __getitems__")

            def __getitems__(self, indices):
                calls.append(list(indices))
                return rumi.read_many(
                    [paths[i] for i in indices],
                    [headers[i] for i in indices],
                    windows=[windows[i] for i in indices],
                    framework="torch",
                )

        def identity(batch):
            return batch

        got = list(torch.utils.data.DataLoader(
            Dataset(), batch_size=3, num_workers=0, collate_fn=identity
        ))
        assert calls == [[0, 1, 2], [3]]
        assert [tuple(batch.shape) for batch in got] == [
            (3, 3, 32, 32), (1, 3, 32, 32)
        ]
        assert np.array_equal(got[0][0].numpy(), square[0][2][:, :32, :32])


class TestRejections:
    @pytest.fixture
    def pair(self, square):
        return ([p for p, _h, _d in square][:2],
                [h for _p, h, _d in square][:2])

    def test_a_single_source_is_sent_to_read(self, square):
        path, header, _data = square[0]
        with pytest.raises(TypeError, match="sequence of sources"):
            rumi.read_many(path, header, windows=[(0, 0, 32, 32)])

    def test_windows_must_match_the_sources(self, pair):
        paths, headers = pair
        with pytest.raises(ValueError, match="sources and windows length"):
            rumi.read_many(paths, headers, windows=[(0, 0, 32, 32)])

    def test_headers_must_match_the_sources(self, pair):
        paths, headers = pair
        with pytest.raises(ValueError, match="length mismatch"):
            rumi.read_many(paths, headers[:1], windows=[(0, 0, 32, 32)] * 2)

    def test_one_bytes_header_is_not_a_batch(self, pair):
        paths, headers = pair
        with pytest.raises(TypeError, match="one header per source"):
            rumi.read_many(paths, headers[0], windows=[(0, 0, 32, 32)] * 2)

    def test_every_window_must_be_the_same_size(self, pair):
        paths, headers = pair
        with pytest.raises(ValueError, match="same size"):
            rumi.read_many(paths, headers,
                            windows=[(0, 0, 32, 32), (0, 0, 16, 16)])

    def test_an_empty_batch_is_refused(self, pair):
        _paths, _headers = pair
        with pytest.raises(ValueError, match="at least one"):
            rumi.read_many([], [], windows=[])

    @pytest.mark.parametrize(
        "window, message",
        [((-1, 0, 32, 32), "negative"),
         ((0, -1, 32, 32), "negative"),
         ((0, 0, 0, 32), "positive"),
         ((0, 0, 32, 0), "positive"),
         ((0, 0, 32), "expected \\(row, column, height, width\\)")],
    )
    def test_a_malformed_window_is_named(self, pair, window, message):
        paths, headers = pair
        with pytest.raises((ValueError, TypeError), match=message):
            rumi.read_many(paths, headers,
                            windows=[(0, 0, 32, 32), window])

    def test_a_window_is_checked_against_its_own_item(self, scenes):
        square_path, square_header, _d = scenes[0]
        wide_path, wide_header, _w = scenes[4]
        window = (0, 128, 32, 32)
        rumi.read_many([wide_path], [wide_header], windows=[window])
        with pytest.raises(ValueError, match="out of bounds"):
            rumi.read_many([wide_path, square_path],
                            [wide_header, square_header],
                            windows=[window, window])

    def test_incompatible_items_are_refused(self, tmp_path, square):
        rng = np.random.default_rng(1)
        square_path, square_header, _d = square[0]

        wrong_bands = rng.integers(0, 3000, (2, 96, 96)).astype(np.uint16)
        bands_path, bands_header = _write(tmp_path / "bands.rumi", wrong_bands)
        wrong_dtype = rng.integers(0, 200, (3, 96, 96)).astype(np.uint8)
        dtype_path, dtype_header = _write(tmp_path / "dtype.rumi", wrong_dtype)

        for path, header in ((bands_path, bands_header),
                             (dtype_path, dtype_header)):
            with pytest.raises(ValueError, match="mismatch"):
                rumi.read_many([square_path, str(path)],
                                [square_header, header],
                                windows=[(0, 0, 32, 32)] * 2)


class TestThreads:
    def test_concurrent_batches_do_not_interfere(self, square):
        paths = [p for p, _h, _d in square]
        headers = [h for _p, h, _d in square]
        windows = [(0, 0, 32, 32), (32, 32, 32, 32), (64, 0, 32, 32),
                   (0, 64, 32, 32)]
        expected = rumi.read_many(paths, headers, windows=windows)

        def once(_):
            return rumi.read_many(paths, headers, windows=windows)

        with ThreadPoolExecutor(8) as pool:
            for result in pool.map(once, range(32)):
                assert np.array_equal(result, expected)
