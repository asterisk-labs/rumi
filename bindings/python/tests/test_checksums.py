"""OpenZL checksum configuration."""

import os
import subprocess
import sys
import textwrap

import numpy as np
import pytest
import rumi
from _labels import labels

geozl = pytest.importorskip("geozl")

GRAPH = "planar>zigzag>zstd"


@pytest.fixture(scope="module")
def image(tmp_path_factory):
    """Create a small file to decode."""
    rng = np.random.default_rng(0)
    data = rng.integers(0, 3000, (2, 64, 64)).astype(np.uint16)
    tf = rumi.frames(data, "b (row h) (col w) -> row col b (h w)", 32)
    graphs = {}
    for t in tf:
        g = graphs.get(t.data.shape)
        if g is None:
            g = graphs[t.data.shape] = geozl.graph(t.data, GRAPH)
        t.compressed = geozl.compress(t.data, graph=g)
    path = tmp_path_factory.mktemp("checksums") / "img.rumi"
    path, _ = rumi.write(path, tf, **labels(tf))
    return str(path)


def run(image, body, **env):
    """Run code in a fresh interpreter and return stdout."""
    src = textwrap.dedent(f"""
        import warnings
        import numpy as np
        import rumi
        PATH = {image!r}
        HDR = rumi.info(source=PATH).header
    """) + textwrap.dedent(body)
    child_env = {**os.environ, "RUMI_VERIFY": "", **env}
    done = subprocess.run([sys.executable, "-c", src], capture_output=True,
                          text=True, timeout=180, env=child_env)
    assert done.returncode == 0, done.stderr
    return done.stdout.strip()


def test_checksum_verification_is_off_by_default(image):
    assert run(
        image, "print(rumi.get_checksum_verification())", RUMI_VERIFY=""
    ) == "False"


@pytest.mark.parametrize("value", ["1", "true", "on", "yes"])
def test_the_environment_turns_it_on(image, value):
    assert run(
        image, "print(rumi.get_checksum_verification())", RUMI_VERIFY=value
    ) == "True"


@pytest.mark.parametrize("value", ["0", "false", "off", "no"])
def test_the_environment_turns_it_off(image, value):
    assert run(
        image, "print(rumi.get_checksum_verification())", RUMI_VERIFY=value
    ) == "False"


@pytest.mark.parametrize("value", ["True", "maybe", "2"])
def test_an_invalid_environment_is_an_error(image, value):
    assert run(image, """
        try:
            rumi.get_checksum_verification()
        except ValueError as error:
            print(error)
    """, RUMI_VERIFY=value) == (
        "RUMI_VERIFY must be 0, 1, false, true, off, on, no, or yes")


def test_an_explicit_setting_replaces_an_invalid_environment(image):
    assert run(image, """
        print(rumi.set_checksum_verification(True),
              rumi.get_checksum_verification())
    """, RUMI_VERIFY="invalid") == "True True"


def test_an_invalid_environment_prevents_a_read(image):
    assert run(image, """
        try:
            rumi.read(PATH, HDR)
        except ValueError as error:
            print(error)
    """, RUMI_VERIFY="invalid") == (
        "RUMI_VERIFY must be 0, 1, false, true, off, on, no, or yes")


def test_set_before_any_read(image):
    assert run(image, """
        print(rumi.set_checksum_verification(True),
              rumi.get_checksum_verification())
    """) == "True True"


def test_the_setting_survives_a_round_trip(image):
    assert run(image, """
        rumi.set_checksum_verification(True)
        print(rumi.set_checksum_verification(False),
              rumi.get_checksum_verification())
    """) == "False False"


def test_a_read_pins_the_setting_and_rejects_a_later_change(image):
    assert run(image, """
        rumi.read(PATH, HDR)
        try:
            rumi.set_checksum_verification(True)
        except ValueError as error:
            print(error)
    """) == "checksum verification is pinned at 0, not 1"


@pytest.mark.parametrize("value", [0, 1, None, "yes"])
def test_python_requires_a_boolean(value):
    with pytest.raises(TypeError, match="must be a bool"):
        rumi.set_checksum_verification(value)


def test_a_checksum_verified_read_returns_the_same_data(image):
    assert run(image, """
        plain = rumi.read(PATH, HDR)
        print(plain.shape, plain.dtype, int(plain.sum()))
    """) == run(image, """
        rumi.set_checksum_verification(True)
        checked = rumi.read(PATH, HDR)
        print(checked.shape, checked.dtype, int(checked.sum()))
    """)


def test_the_environment_survives_into_a_read(image):
    assert run(image, """
        rumi.read(PATH, HDR)
        print(rumi.get_checksum_verification())
    """, RUMI_VERIFY="1") == "True"
