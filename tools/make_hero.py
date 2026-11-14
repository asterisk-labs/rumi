"""Make the images and frame table of the landing page figure.

Reads a 1280 pixel crop of Lake Annecy in four Sentinel-2 bands, writes it as a
rumi file of 256 pixel tiles with the smaller GPU recipe for every frame, and
saves one tinted image per band and a true-colour image to docs/assets/hero.
Prints the FRAMES table to paste into docs/assets/js/reader.js. Needs rasterio,
Pillow and network access.
"""

import tempfile
from pathlib import Path

import geozl
import numpy as np
import rasterio
import rumi
from PIL import Image
from rasterio.windows import Window

SCENE = (
    "https://sentinel-cogs.s3.us-west-2.amazonaws.com/sentinel-s2-l2a-cogs/"
    "31/T/GL/2025/8/S2A_31TGL_20250808_0_L2A/"
)
ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "docs" / "assets" / "hero"
WINDOW = Window(3980, 1244, 1280, 1280)
BANDS = ["B02", "B03", "B04", "B08"]
TONES = [(38, 99, 235), (22, 163, 74), (220, 38, 38), (73, 42, 232)]
RECIPES = {"planar>zigzag>pfor": "p", "planar>zigzag>lanes": "l"}
TILE = 256


def fetch():
    with rasterio.Env(GDAL_DISABLE_READDIR_ON_OPEN="EMPTY_DIR", AWS_NO_SIGN_REQUEST="YES"):
        bands = []
        for name in BANDS:
            with rasterio.open(SCENE + name + ".tif") as src:
                bands.append(src.read(1, window=WINDOW))
    return np.stack(bands)


def frame_table(x):
    tf = rumi.frames(x, "b (row h) (col w) -> row col b (h w)", tile_size=TILE)
    chosen = {}
    for frame in tf:
        payloads = {
            g: geozl.compress(frame.data, graph=geozl.graph(frame.data, g)) for g in RECIPES
        }
        recipe = min(payloads, key=lambda g: len(payloads[g]))
        frame.compressed = payloads[recipe]
        chosen[frame.band, frame.row, frame.col] = (
            RECIPES[recipe],
            len(payloads[recipe]),
        )
    with tempfile.TemporaryDirectory() as tmp:
        path, header = rumi.write(Path(tmp) / "annecy.rumi", tf, bands=BANDS, time=["2025-08-08"])
        back = rumi.read(path, header, window=(TILE, TILE, TILE, TILE))
        assert np.array_equal(back, x[:, TILE : 2 * TILE, TILE : 2 * TILE])
    rows = []
    grid = x.shape[1] // TILE
    for band in range(len(BANDS)):
        cells = [chosen[band, r, c] for r in range(grid) for c in range(grid)]
        recipes = "".join(letter for letter, _ in cells)
        sizes = ", ".join(str(size) for _, size in cells)
        rows.append(f"    ['{recipes}', [{sizes}]]")
    return ",\n".join(rows), len(header)


def stretch(band):
    lo, hi = np.percentile(band, [1, 99])
    return np.clip((band - lo) / (hi - lo), 0, 1)


def images(x):
    OUT.mkdir(parents=True, exist_ok=True)
    for band, name, tone in zip(x.astype(np.float32), BANDS, TONES, strict=True):
        tone = np.array(tone, np.float32)
        dark, light = tone * 0.18, tone + (255 - tone) * 0.82
        tinted = dark + stretch(band)[..., None] ** 0.8 * (light - dark)
        image = Image.fromarray(tinted.astype(np.uint8)).resize((560, 560), Image.LANCZOS)
        image.save(OUT / f"{name.lower()}.webp", quality=68, method=6)
    rgb = np.stack([x[2], x[1], x[0]], -1).astype(np.float32)
    rgb = np.clip((rgb - 100) / 1500, 0, 1) ** (1 / 1.6)
    image = Image.fromarray((rgb * 255).astype(np.uint8)).resize((800, 800), Image.LANCZOS)
    image.save(OUT / "rgb.webp", quality=74, method=6)


def main():
    x = fetch()
    images(x)
    table, header = frame_table(x)
    print(f"// header {header} bytes")
    print(table)


if __name__ == "__main__":
    main()
