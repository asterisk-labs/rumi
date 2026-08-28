"""Generate structurally valid rumi files and external headers for fuzzing.

The corpus covers every frame layout, both index orders, time trailers, and
georeferencing. Frame payloads are placeholders because indexing validates
metadata without decoding frames.
"""

import pathlib
import shutil
import sys

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]
                       / "bindings" / "python"))

import rumi                                    # noqa: E402
from rumi._write import write_frames           # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parents[1]
UTM = (10.0, 0.0, 500000.0, 0.0, -10.0, 4600000.0)

CASES = [
    ("image_tile",    (1, 16, 16),    8, "b (row h) (col w) -> row col b (h w)", {}),
    ("image_planar",  (3, 16, 16),    8, "b (row h) (col w) -> row col (b h w)",
     {"transform": UTM, "crs": 32630}),
    ("image_chunky",  (3, 16, 16),    8, "b (row h) (col w) -> row col (h w b)", {}),
    ("image_dated",   (2, 16, 16),    8, "b (row h) (col w) -> row col (b h w)",
     {"time": ["2024-08-25"]}),
    ("image_ragged",  (2, 20, 26),    8, "b (row h) (col w) -> row col (b h w)", {}),
    ("cube_bt",       (3, 2, 16, 16), 8, "t b (row h) (col w) -> row col (b t h w)",
     {"time": ["2024-05-01", "2024-06-01", "2024-07-01"]}),
    ("cube_tb",       (3, 2, 16, 16), 8, "t b (row h) (col w) -> row col (t b h w)", {}),
    ("cube_index_bt", (3, 2, 16, 16), 8, "t b (row h) (col w) -> row col b t (h w)", {}),
    ("cube_index_tb", (3, 2, 16, 16), 8, "t b (row h) (col w) -> row col t b (h w)", {}),
    ("cube_chunky",   (3, 2, 16, 16), 8, "t b (row h) (col w) -> row col (h w t b)", {}),
    ("cube_spans",    (2, 2, 16, 16), 8, "t b (row h) (col w) -> row col (b t h w)",
     {"time": [("2024-05-01", "2024-06-01"), ("2024-07-01", "2024-08-01")]}),
    ("cube_one_band", (4, 1, 16, 16), 8, "t b (row h) (col w) -> row col (t b h w)",
     {"time": ["2024-05-01", "2024-06-01", "2024-07-01", "2024-08-01"]}),
    ("cube_clocked",  (2, 1, 16, 16), 8, "t b (row h) (col w) -> row col (t b h w)",
     {"time": ["2024-05-01T06:30:00Z", "2024-05-02T06:31:00Z"]}),
]


def build(out_index: pathlib.Path, out_header: pathlib.Path) -> int:
    for name, shape, tile, pattern, kw in CASES:
        arr = np.arange(int(np.prod(shape)), dtype=np.uint16).reshape(shape)
        tf = rumi.frames(arr, pattern, tile)
        # Distinct lengths expose frame-order permutations.
        tf["compressed"] = [bytes([i % 251]) * (3 + i % 7) for i in range(len(tf))]
        path = out_index / name
        blob = write_frames(path, tf["compressed"], tf, **kw)
        (out_header / name).write_bytes(blob)
    return len(CASES)


def main() -> int:
    seeds = ROOT / "fuzz" / "replay"
    for target in ("index", "header"):
        d = seeds / target
        for old in d.glob("seed_*"):
            old.unlink()
    index_dir, header_dir = seeds / "index", seeds / "header"
    index_dir.mkdir(parents=True, exist_ok=True)
    header_dir.mkdir(parents=True, exist_ok=True)

    staged = ROOT / "fuzz" / "replay" / ".staging"
    if staged.exists():
        shutil.rmtree(staged)
    (staged / "index").mkdir(parents=True)
    (staged / "header").mkdir(parents=True)
    n = build(staged / "index", staged / "header")
    for target in ("index", "header"):
        for f in sorted((staged / target).iterdir()):
            (seeds / target / f"seed_{f.name}").write_bytes(f.read_bytes())
    shutil.rmtree(staged)
    print(f"wrote {n} seeds each into {index_dir} and {header_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
