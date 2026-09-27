"""Freeze the public Python surface independently of private modules."""

import inspect
import subprocess
import sys

import rumi

PUBLIC = (
    "DType",
    "Frame",
    "FrameTable",
    "Metadata",
    "RumiArray",
    "frame_start",
    "frames",
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
        "frames": ("arr", "pattern", "tile_size"),
        "get_checksum_verification": (),
        "get_num_threads": (),
        "info": ("source", "header"),
        "read": ("source", "header", "framework", "pattern", "time", "bands", "window"),
        "read_many": (
            "sources", "headers", "windows", "framework", "pattern", "time", "bands"
        ),
        "set_checksum_verification": ("on",),
        "set_num_threads": ("n",),
        "write": ("path", "tf", "bands", "time", "transform", "crs", "pixel_is_point"),
    }
    for name, parameters in expected.items():
        assert tuple(inspect.signature(getattr(rumi, name)).parameters) == parameters


def test_read_defaults_stay_stable():
    read = inspect.signature(rumi.read).parameters
    assert read["header"].default is inspect.Parameter.empty
    assert read["framework"].default == "numpy"
    assert all(read[name].default is None for name in ("pattern", "time", "bands", "window"))

    many = inspect.signature(rumi.read_many).parameters
    assert many["headers"].default is inspect.Parameter.empty
    assert many["windows"].default is inspect.Parameter.empty
    assert many["framework"].default == "numpy"
    assert all(many[name].default is None for name in ("pattern", "time", "bands"))
