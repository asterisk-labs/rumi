import os
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[3]
CHECK = ROOT / "tools" / "check_editable.py"

if not CHECK.is_file():
    pytest.skip(
        "editable-install check is only available in a source checkout",
        allow_module_level=True,
    )


def run_check(**env):
    return subprocess.run(
        [sys.executable, CHECK],
        cwd=ROOT,
        env={**os.environ, **env},
        capture_output=True,
        text=True,
    )


def test_editable_check_accepts_this_checkout():
    result = run_check()
    assert result.returncode == 0, result.stderr
    assert (ROOT / "VERSION").read_text().strip() in result.stdout


def test_editable_check_rejects_another_rumi_module(tmp_path):
    other = tmp_path / "rumi.py"
    other.write_text("__version__ = '0.1'\n")

    result = run_check(PYTHONPATH=str(tmp_path))

    assert result.returncode == 1
    assert str(other) in result.stderr
    assert "expected" in result.stderr
