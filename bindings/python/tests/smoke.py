"""Post-install check for a built wheel, run against a clean venv.

Writes a georeferenced file and reads its header back. That covers the writer,
the bundled proj.db a CRS needs, and the header round trip, without geozl and
without a GDAL on the host. Tile payloads are fake, nothing decodes them here.
"""

import sys
import tempfile
from pathlib import Path

import numpy as np

import rumi

CRS = 32630
TRANSFORM = (10.0, 0.0, 500000.0, 0.0, -10.0, 4600000.0)


def main() -> int:
    print("rumi", rumi.__version__)

    tf = rumi.tile(np.zeros((2, 40, 70), np.uint16), 16)
    tf["compressed"] = [bytes([i % 251]) * (8 + i) for i in range(len(tf))]

    with tempfile.TemporaryDirectory() as d:
        path, header = rumi.write(Path(d) / "smoke.tif", tf,
                                  transform=TRANSFORM, crs=CRS)
        facts = rumi.RumiHeader(header).to_dict()
        on_disk = rumi.RumiHeader.from_path(path).to_dict()

    if facts != on_disk:
        print("::error::the header written and the header read back differ")
        return 1
    if facts["shape"] != [2, 40, 70] or facts["dtype"] != "uint16":
        print(f"::error::header says {facts}")
        return 1

    print(f"wrote and re-read EPSG:{CRS}, {facts['tiles']} tiles, "
          f"{len(header)} byte header")
    return 0


if __name__ == "__main__":
    sys.exit(main())
