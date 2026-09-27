import datetime as dt
import re
import threading
from dataclasses import fields
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

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
    assert isinstance(metadata.dtype, rumi.DType)
    assert metadata.dtype.name == "uint16"
    assert metadata.dtype.itemsize == metadata.dtype.component_size == 2
    assert metadata.dtype.numpy_dtype is np.uint16
    assert metadata.tile == (16, 16)
    assert metadata.frame_layout == "b h w"
    assert metadata.index_order == ()
    assert metadata.bands == ["red", "nir"]
    assert metadata.time == [dt.date(2024, 8, 25)]
    assert metadata.time_kind == "instant"
    assert metadata.transform == TRANSFORM
    assert metadata.crs == 32718
    assert metadata.pixel_is_point is False


def many_frames(tmp_path):
    # 2000 frames need a 24452-byte header region, past the 16 KiB first read.
    data = np.zeros((1, 640, 800), dtype=np.uint16)
    table = rumi.frames(
        data, "b (row h) (col w) -> row col b (h w)", tile_size=16)
    graph = geozl.graph(table[0].data, "planar>zigzag>zstd")
    for frame in table:
        frame.compressed = geozl.compress(frame.data, graph=graph)
    return rumi.write(tmp_path / "many.rumi", table, bands=["red"],
                      time=["2024-08-25"])


@pytest.fixture
def served():
    """Serve files over HTTP ranges and record each request.

    With suffix=False the server answers a suffix range with the whole object,
    as Azure does.
    """
    objects = {}
    requests = []
    options = {"suffix": True}

    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def do_GET(self):
            payload = objects[self.path]
            spec = self.headers.get("Range", "")
            requests.append((spec, self.headers.get("If-Match")))
            suffix = re.fullmatch(r"bytes=-(\d+)", spec)
            status, first, last = 206, 0, len(payload) - 1
            if suffix and options["suffix"]:
                first = max(0, len(payload) - int(suffix.group(1)))
            elif suffix:
                status = 200
            else:
                bounds = re.fullmatch(r"bytes=(\d+)-(\d+)", spec)
                first = int(bounds.group(1))
                last = min(int(bounds.group(2)), last)
            body = payload[first:last + 1]
            self.send_response(status)
            if status == 206:
                self.send_header("Content-Range",
                                 f"bytes {first}-{last}/{len(payload)}")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("ETag", '"v1"')
            self.end_headers()
            try:
                self.wfile.write(body)
            except (BrokenPipeError, ConnectionResetError):
                pass

        def log_message(self, _format, *_args):
            pass

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever)
    thread.start()

    def serve(path, suffix=True):
        objects["/" + path.name] = path.read_bytes()
        options["suffix"] = suffix
        requests.clear()
        return f"http://127.0.0.1:{server.server_port}/{path.name}", requests

    yield serve
    server.shutdown()
    server.server_close()
    thread.join()


def test_remote_source_reads_both_ends_at_once(tmp_path, served):
    path, header = stored(tmp_path)
    url, requests = served(path)
    assert rumi.info(source=url).header == header
    assert sorted(requests) == [("bytes=-16384", None), ("bytes=0-16383", None)]


def test_a_long_header_region_takes_one_more_request(tmp_path, served):
    path, header = many_frames(tmp_path)
    url, requests = served(path)
    assert rumi.info(source=url).header == header
    assert sorted(requests[:2]) == [("bytes=-16384", None),
                                    ("bytes=0-16383", None)]
    assert requests[2:] == [("bytes=16384-24451", '"v1"')]


def test_a_server_without_suffix_ranges_gets_the_tail_by_offset(
        tmp_path, served):
    path, header = many_frames(tmp_path)
    size = path.stat().st_size
    url, requests = served(path, suffix=False)
    assert rumi.info(source=url).header == header
    assert (f"bytes={size - 16384}-{size - 1}", '"v1"') in requests


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
