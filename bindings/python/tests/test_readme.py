"""The README's Python blocks, run as written"""

import pathlib
import re

import numpy as np
import pytest
import rumi

geozl = pytest.importorskip("geozl")

README = pathlib.Path(__file__).resolve().parents[3] / "README.md"

# The complete quick start can run by itself. Later blocks intentionally reuse
# names it defines, so compiling them is enough here.
RUNNABLE = ("rumi.frames", "rumi.write")


def blocks():
    if not README.is_file():          # a wheel install has no README beside it
        pytest.skip(f"{README} not present")
    found = re.findall(r"```python\n(.*?)```", README.read_text(), re.S)
    assert found, "the README has no Python blocks, which cannot be right"
    return found


def is_runnable(block):
    return (all(name in block for name in RUNNABLE)
            and "import pandas" not in block
            and "catalog" not in block)


@pytest.mark.parametrize("index", range(len(blocks())))
def test_a_readme_block_at_least_compiles(index):
    compile(blocks()[index], f"README.md block {index}", "exec")


@pytest.mark.parametrize("index", [i for i, b in enumerate(blocks())
                                   if is_runnable(b)])
def test_a_readme_block_runs(index, tmp_path, monkeypatch):
    monkeypatch.chdir(tmp_path)
    env = {"np": np, "geozl": geozl, "rumi": rumi}
    exec(compile(blocks()[index], f"README.md block {index}", "exec"), env)


def test_the_quick_start_writes_a_file_that_reads_back(tmp_path, monkeypatch):
    """The first block is the one a reader copies, so check what it leaves
    behind rather than only that it ran."""
    monkeypatch.chdir(tmp_path)
    env = {"np": np, "geozl": geozl, "rumi": rumi}
    exec(compile(blocks()[0], "README.md quick start", "exec"), env)

    scene = tmp_path / "scene.rumi"
    assert scene.is_file()
    assert np.array_equal(env["result"], env["image"])
    assert np.asarray(env["chip"]).shape == (2, 512, 512)
    assert np.asarray(rumi.read(str(scene))).shape == (4, 1024, 1024)
