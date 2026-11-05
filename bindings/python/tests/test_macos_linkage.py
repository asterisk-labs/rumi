import subprocess
import sys

import pytest


@pytest.mark.skipif(sys.platform != "darwin", reason="Objective-C classes are macOS-only")
def test_rumi_and_geozl_load_without_duplicate_metal_classes():
    script = """
import numpy as np
import geozl
from rumi._ffi import lib

samples = np.arange(16, dtype=np.uint16).reshape(4, 4)
geozl.graph(samples, "planar>zigzag>zstd")
assert lib.rumi_api_version() == 1
"""
    result = subprocess.run(
        [sys.executable, "-c", script], capture_output=True, text=True, check=False
    )
    assert result.returncode == 0, result.stderr
    assert "is implemented in both" not in result.stderr
