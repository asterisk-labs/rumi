#!/usr/bin/env python3
"""Write the small shared fixtures used by the R and Julia tests."""

from __future__ import annotations

import argparse
from datetime import UTC, datetime
from pathlib import Path

import geozl
import numpy as np
import rumi


def frames(data: np.ndarray) -> rumi.FrameTable:
    table = rumi.frames(
        data,
        "t b (row h) (col w) -> row col t b (h w)",
        tile_size=4,
    )
    for frame in table:
        graph = geozl.graph(frame.data, "planar>zigzag>zstd")
        frame.compressed = geozl.compress(frame.data, graph=graph)
    return table


def write(directory: Path, name: str, table: rumi.FrameTable) -> None:
    path = directory / f"{name}.rumi"
    header = rumi.write(
        path,
        table,
        bands=["B02", "B03", "B04"],
        time=[
            datetime(2026, 1, 1, tzinfo=UTC),
            datetime(2026, 1, 6, tzinfo=UTC),
        ],
        transform=(10.0, 0.0, 500000.0, 0.0, -10.0, 8000000.0),
        crs=32618,
    )
    (directory / f"{name}.header").write_bytes(header)
    print(path)


def geozl_cases() -> dict[str, tuple[np.ndarray, str, dict]]:
    """Rasters the R and Julia tests rebuild by formula, and how to compress them."""
    b, y, x = np.indices((3, 48, 40))
    counts = (200 + b * 977 + y * 31 + x * 17
              + (x * 7919 + y * 104729 + b * 31) % 97).astype(np.uint16)
    y, x = np.indices((48, 40))
    reflectance = (((y * 131 + x * 71) % 1000) / 4000 + 0.1).astype(np.float32)
    holes = reflectance.copy()
    holes[(y * 40 + x) % 97 == 0] = np.nan
    # Exercises float16 ties, subnormals and NaNs.
    half = np.where(y == 0, 2048 + x % 8,
                    np.where(y == 1, (x - 20) / 3 * 2.0 ** -14,
                             ((y * 131 + x * 71) % 1000) / 7 - 60)).astype(np.float16)
    half[(y * 40 + x) % 89 == 0] = np.nan
    # int64 samples below 2^53, so R holds them exactly as doubles.
    wide = ((x * 7919 + y * 104729) % 1000 * 10**12 - 5 * 10**14).astype(np.int64)
    wide[(y * 40 + x) % 83 == 0] = -2**63
    y, x = np.indices((128, 128))
    radiance = (50 + 4000 * (x + y) / 254
                + (x * 7919 + y * 104729) % 61).astype(np.float32)
    return {
        "lossless": (counts, "planar>zigzag>zstd", {}),
        "linear": (counts, "planar>zigzag>pfor", {"error": 3}),
        "linear-float": (reflectance, "planar>zigzag>zstd", {"error": 0.01}),
        "log": (reflectance, "planar>zigzag>zstd", {"error": "1%"}),
        "sqrt": (radiance, "planar>zigzag>zstd",
                 {"error": "SQRT:MAX_ERROR=0.5N,A=25,B=2"}),
        "sentinel": (counts, "planar>zigzag>zstd", {"nodata": 200}),
        "nan": (holes, "planar>zigzag>zstd", {}),
        "half": (half, "planar>zigzag>zstd", {}),
        "int64": (wide, "planar>zigzag>zstd", {"nodata": -2**63}),
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("directory", type=Path, nargs="+")
    args = parser.parse_args()

    t, b, y, x = np.indices((2, 3, 6, 8), dtype=np.uint16)
    data = t * 10000 + b * 1000 + y * 100 + x
    integer = frames(data)
    floating = frames(data.astype(np.float64) / 10)

    expected = {name: geozl.compress(raster, graph=geozl.graph(raster, method, **options))
                for name, (raster, method, options) in geozl_cases().items()}

    for directory in args.directory:
        directory.mkdir(parents=True, exist_ok=True)
        write(directory, "binding", integer)
        write(directory, "binding-f64", floating)
        for name, frame in expected.items():
            (directory / f"geozl-{name}.frame").write_bytes(frame)


if __name__ == "__main__":
    main()
