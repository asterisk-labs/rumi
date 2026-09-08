"""Post-install wheel check for writing, indexing, and both byte sources."""

import sys
import tempfile
from pathlib import Path

import numpy as np
import rumi
from rumi._ffi import API_VERSION, ffi, lib

CRS = 32630
TRANSFORM = (10.0, 0.0, 500000.0, 0.0, -10.0, 4600000.0)


def main() -> int:
    print("rumi", rumi.__version__)
    if lib.rumi_api_version() != API_VERSION:
        print(f"::error::librumi C API is {lib.rumi_api_version()}, "
              f"the binding transcribes {API_VERSION}")
        return 1
    native_version = ffi.string(lib.rumi_version_string()).decode("ascii")
    if native_version != rumi.__version__:
        print(
            f"::error::wheel metadata is {rumi.__version__}, "
            f"but librumi is {native_version}"
        )
        return 1

    tf = rumi.frames(np.zeros((2, 40, 70), np.uint16), "b (row h) (col w) -> row col b (h w)", 16)
    tf["compressed"] = [bytes([i % 251]) * (8 + i) for i in range(len(tf))]

    with tempfile.TemporaryDirectory() as d:
        path, header = rumi.write(Path(d) / "smoke.rumi", tf,
                                  transform=TRANSFORM, crs=CRS)
        facts = rumi.info(header=header)
        on_disk = rumi.info(source=path)
        blob = Path(path).read_bytes()

    if facts.header != on_disk.header:
        print("::error::the header written and the header read back differ")
        return 1
    if facts.shape != (2, 40, 70) or facts.dtype is not np.uint16:
        print(f"::error::header says {facts}")
        return 1

    print(f"wrote and re-read EPSG:{CRS}, {facts['frames']} frames, "
          f"{len(header)} byte header, {len(blob)} byte file")
    return 0


if __name__ == "__main__":
    sys.exit(main())
