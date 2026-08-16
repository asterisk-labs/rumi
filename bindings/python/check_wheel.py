"""Check metadata, licence notices, and native libraries in RUMI wheels.

    python bindings/python/check_wheel.py dist/*.whl
"""

import sys
import zipfile
from email.parser import Parser
from pathlib import Path

NOTICES = (
    "LICENSE",
    "NOTICE",
    "LICENSE.GeoZL",
    "LICENSE.OpenZL",
    "LICENSE.Zstandard",
    "LICENSE.LZ4",
)
LIB_SUFFIXES = (".so", ".dylib", ".dll")
EXPECTED_NAME = "rumi-eo"
EXPECTED_VERSION = (Path(__file__).resolve().parents[2] / "VERSION").read_text().strip()


def check(path: Path) -> list[str]:
    bad: list[str] = []
    with zipfile.ZipFile(path) as wheel:
        names = set(wheel.namelist())
        metadata_names = [name for name in names if name.endswith(".dist-info/METADATA")]
        if len(metadata_names) != 1:
            return [f"expected one dist-info/METADATA, found {sorted(metadata_names)}"]
        metadata = Parser().parsestr(wheel.read(metadata_names[0]).decode("utf-8"))

    info = metadata_names[0].split("/")[0]
    if metadata["Name"] != EXPECTED_NAME:
        bad.append(f"metadata Name is {metadata['Name']!r}, expected {EXPECTED_NAME!r}")
    if metadata["Version"] != EXPECTED_VERSION:
        bad.append(
            f"metadata Version is {metadata['Version']!r}, expected {EXPECTED_VERSION!r}"
        )

    for notice in NOTICES:
        want = f"{info}/licenses/{notice}"
        if want not in names:
            bad.append(f"missing {want}")

    libs = [
        name
        for name in names
        if name.startswith("rumi/_lib/") and name.endswith(LIB_SUFFIXES)
    ]
    if len(libs) != 1:
        bad.append(f"expected one native library under rumi/_lib/, found {sorted(libs)}")

    return bad


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2

    failed = False
    for arg in argv[1:]:
        path = Path(arg)
        bad = check(path)
        for line in bad:
            print(f"{path.name}: {line}", file=sys.stderr)
        if bad:
            failed = True
        else:
            print(f"{path.name}: metadata, notices, and native library present")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
