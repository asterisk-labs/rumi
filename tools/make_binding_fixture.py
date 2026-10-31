#!/usr/bin/env python3
"""Write the small shared fixture used by the R and Julia reader tests."""

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
    path, header = rumi.write(
        directory / f"{name}.rumi",
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


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("directory", type=Path, nargs="+")
    args = parser.parse_args()

    t, b, y, x = np.indices((2, 3, 6, 8), dtype=np.uint16)
    data = t * 10000 + b * 1000 + y * 100 + x
    integer = frames(data)
    floating = frames(data.astype(np.float64) / 10)

    for directory in args.directory:
        directory.mkdir(parents=True, exist_ok=True)
        write(directory, "binding", integer)
        write(directory, "binding-f64", floating)


if __name__ == "__main__":
    main()
