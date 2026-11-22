"""Freeze the public Python surface independently of private modules."""

import inspect
import pickle
import subprocess
import sys

import pytest
import rumi
from rumi._ffi import _check, lib

PUBLIC = (
    "DType",
    "Frame",
    "FrameTable",
    "Metadata",
    "RumiArray",
    "RumiError",
    "frame_start",
    "frames",
    "geozl",
    "get_checksum_verification",
    "get_num_threads",
    "info",
    "read",
    "read_many",
    "set_checksum_verification",
    "set_num_threads",
    "write",
    "__version__",
)


def test_public_names_are_explicit():
    assert tuple(rumi.__all__) == PUBLIC
    assert all(hasattr(rumi, name) for name in PUBLIC)


def test_import_keeps_optional_frameworks_lazy():
    code = (
        "import sys, rumi; "
        "print(','.join(name for name in ('torch', 'jax', 'tensorflow') "
        "if name in sys.modules))"
    )
    done = subprocess.run(
        [sys.executable, "-c", code], capture_output=True, text=True, check=True)
    assert done.stdout == "\n"


def test_public_function_parameters_stay_stable():
    expected = {
        "frame_start": ("shape", "pattern", "tile_size"),
        "frames": ("x", "pattern", "tile_size"),
        "get_checksum_verification": (),
        "get_num_threads": (),
        "info": ("source", "header"),
        "read": (
            "source", "header", "time", "bands", "window", "pattern", "framework",
            "device", "decode_backend",
        ),
        "read_many": (
            "sources", "headers", "windows", "time", "bands", "pattern", "framework",
            "device", "decode_backend",
        ),
        "set_checksum_verification": ("on",),
        "set_num_threads": ("n",),
        "write": ("path", "frames", "bands", "time", "transform", "crs", "pixel_is_point"),
    }
    for name, parameters in expected.items():
        assert tuple(inspect.signature(getattr(rumi, name)).parameters) == parameters


def test_read_defaults_stay_stable():
    read = inspect.signature(rumi.read).parameters
    assert read["header"].default is inspect.Parameter.empty
    assert read["framework"].default == "numpy"
    assert all(read[name].default is None
               for name in ("pattern", "time", "bands", "window", "device"))
    assert read["decode_backend"].default == "auto"

    many = inspect.signature(rumi.read_many).parameters
    assert many["headers"].default is inspect.Parameter.empty
    assert many["framework"].default == "numpy"
    assert all(many[name].default is None
               for name in ("windows", "pattern", "time", "bands", "device"))
    assert many["decode_backend"].default == "auto"


@pytest.mark.parametrize("status, kind, builtin", [
    ("RUMI_ERR_IO", "io", OSError),
    ("RUMI_ERR_PARSE", "parse", ValueError),
    ("RUMI_ERR_FORMAT", "format", ValueError),
    ("RUMI_ERR_DECODE", "decode", OSError),
    ("RUMI_ERR_OOM", "oom", MemoryError),
    ("RUMI_ERR_UNSUPPORTED", "unsupported", NotImplementedError),
    ("RUMI_ERR_STATE", "state", RuntimeError),
    ("RUMI_ERR_INTERNAL", "internal", RuntimeError),
])
def test_every_failure_status_raises_its_kind(status, kind, builtin):
    with pytest.raises(rumi.RumiError) as raised:
        _check(getattr(lib, status))
    assert raised.value.kind == kind
    assert isinstance(raised.value, builtin)
    # DataLoader workers send exceptions to the parent by pickling them.
    again = pickle.loads(pickle.dumps(raised.value))
    assert type(again) is type(raised.value)
    assert str(again) == str(raised.value)


def test_an_invalid_request_is_a_plain_value_error():
    with pytest.raises(ValueError) as raised:
        _check(lib.RUMI_ERR_INVALID)
    assert type(raised.value) is ValueError
