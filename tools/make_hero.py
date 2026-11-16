"""Make the Lake Annecy example of the landing page and the docs.

Reads a 1280 pixel crop of Lake Annecy in four Sentinel-2 bands, writes it as a
rumi file of 256 pixel tiles with its CRS and transform and the smaller GPU
recipe for every frame, and saves one tinted image per band and a true-colour
image to docs/assets/hero. Writes the facts of that file, from its frame sizes
to its header, to docs/assets/js/annecy.js. Needs rasterio, Pillow and network
access.
"""

import math
import struct
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
DATA = ROOT / "docs" / "assets" / "js" / "annecy.js"
WINDOW = Window(3980, 1244, 1280, 1280)
BANDS = ["B02", "B03", "B04", "B08"]
DATE = "2025-08-08"
TONES = [(38, 99, 235), (22, 163, 74), (220, 38, 38), (73, 42, 232)]
RECIPES = {"planar>zigzag>pfor": "p", "planar>zigzag>lanes": "l"}
TILE = 256


def fetch():
    with rasterio.Env(GDAL_DISABLE_READDIR_ON_OPEN="EMPTY_DIR", AWS_NO_SIGN_REQUEST="YES"):
        bands = []
        for name in BANDS:
            with rasterio.open(SCENE + name + ".tif") as src:
                bands.append(src.read(1, window=WINDOW))
                transform = tuple(src.window_transform(WINDOW))[:6]
                epsg = src.crs.to_epsg()
    return np.stack(bands), transform, epsg


def write(x, transform, epsg):
    tf = rumi.frames(x, "b (row h) (col w) -> row col b (h w)", tile_size=TILE)
    chosen = {}
    for frame in tf:
        payloads = {
            g: geozl.compress(frame.data, graph=geozl.graph(frame.data, g)) for g in RECIPES
        }
        recipe = min(payloads, key=lambda g: len(payloads[g]))
        frame.compressed = payloads[recipe]
        chosen[frame.band, frame.row, frame.col] = (RECIPES[recipe], len(payloads[recipe]))
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "annecy.rumi"
        header = rumi.write(
            path,
            tf,
            bands=BANDS,
            time=[DATE],
            transform=transform,
            crs=epsg,
        )
        back = rumi.read(path, header, window=(TILE, TILE, TILE, TILE))
        assert np.array_equal(back, x[:, TILE : 2 * TILE, TILE : 2 * TILE])
        size = path.stat().st_size
    return chosen, bytes(header), size


def script(x, transform, epsg, chosen, header, size):
    grid = math.ceil(x.shape[1] / TILE)
    rows = []
    for band in range(len(BANDS)):
        cells = [chosen[band, r, c] for r in range(grid) for c in range(grid)]
        recipes = "".join(letter for letter, _ in cells)
        sizes = ", ".join(str(n) for _, n in cells)
        rows.append(f"      ['{recipes}', [{sizes}]]")
    count_min, count_bits = struct.unpack("<IB", header[27:32])
    frames = ",\n".join(rows)
    return f"""// The Lake Annecy file of the landing page and the docs, written by
// tools/make_hero.py. Four Sentinel-2 bands of {x.shape[2]} by {x.shape[1]} pixels in tiles of
// {TILE}, one band per frame, each frame with the smaller GPU recipe.

(function (rumi) {{
  'use strict';

  rumi.annecy = {{
    bands: {BANDS!r},
    date: '{DATE}',
    width: {x.shape[2]},
    height: {x.shape[1]},
    tile: {TILE},
    epsg: {epsg},
    transform: {list(transform)!r},
    fileSize: {size},
    headerSize: {len(header)},
    countMin: {count_min},
    countBits: {count_bits},
    recipes: {{ p: 'planar>zigzag>pfor', l: 'planar>zigzag>lanes' }},
    // per band, row by row: the recipe each frame chose and its size in bytes
    frames: [
{frames}
    ]
  }};
}})((window.rumi = window.rumi || {{}}));
""".replace('"', "'")


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
    x, transform, epsg = fetch()
    images(x)
    chosen, header, size = write(x, transform, epsg)
    DATA.write_text(script(x, transform, epsg, chosen, header, size), encoding="utf-8")
    print(f"wrote {DATA.relative_to(ROOT)}, header {len(header)} bytes, file {size} bytes")


if __name__ == "__main__":
    main()
