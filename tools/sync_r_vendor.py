#!/usr/bin/env python3
"""Copy or check the native sources built by the R source package."""

import argparse
import hashlib
import os
import shutil
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = ROOT / "bindings" / "r"
OPENZL = ROOT / "extern" / "geozl" / "extern" / "openzl"
MANAGED = (Path("src/ext"), Path("inst/NOTICE"), Path("inst/licenses"))


def copy(source: Path, destination: Path, *skip: str) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    if source.is_dir():
        shutil.copytree(
            source,
            destination,
            dirs_exist_ok=True,
            ignore=shutil.ignore_patterns(".*", "*.py", *skip),
        )
    else:
        shutil.copy2(source, destination)


def copy_openzl(destination: Path) -> None:
    for part in ("CMakeLists.txt", "LICENSE", "build-scripts", "include", "src"):
        copy(OPENZL / part, destination / part)
    copy(OPENZL / "tools" / "CMakeLists.txt", destination / "tools" / "CMakeLists.txt")
    for listfile in (OPENZL / "tools").glob("*/CMakeLists.txt"):
        copy(listfile, destination / listfile.relative_to(OPENZL))
    copy(OPENZL / "tools" / "fileio", destination / "tools" / "fileio")
    for name in ("profile_graphs.cpp", "profile_graphs.h"):
        source = OPENZL / "cli" / "utils" / name
        if source.exists():
            copy(source, destination / "cli" / "utils" / name)
    for dependency in ("zstd", "lz4"):
        for part in ("lib", "build/cmake", "LICENSE", "COPYING", "README.md"):
            source = OPENZL / "deps" / dependency / part
            if source.exists():
                copy(
                    source,
                    destination / "deps" / dependency / part,
                    "Makefile",
                    "dll",
                )


def sync(package: Path) -> None:
    vendor = package / "src" / "ext"

    shutil.rmtree(vendor, ignore_errors=True)
    copy(ROOT / "VERSION", vendor / "VERSION")
    for part in ("CMakeLists.txt", "include", "src"):
        copy(ROOT / "core" / part, vendor / "r" / part)

    geozl = ROOT / "extern" / "geozl"
    for part in ("CMakeLists.txt", "include", "src"):
        skip = ("gpu",) if part == "src" else ()
        copy(geozl / "core" / part, vendor / "g" / "core" / part, *skip)
    copy(
        geozl / "core" / "src" / "gpu" / "unavailable.c",
        vendor / "g" / "core" / "src" / "gpu" / "unavailable.c",
    )
    copy_openzl(vendor / "z")

    karu = ROOT / "extern" / "karu"
    for part in (
        "CMakeLists.txt",
        "VERSION",
        "LICENSE",
        "README.md",
        "CHANGELOG.md",
        "CONFIGURATION.md",
        "cmake",
        "include",
        "src",
    ):
        copy(karu / part, vendor / "k" / part)

    (package / "inst").mkdir(exist_ok=True)
    shutil.copy2(ROOT / "NOTICE", package / "inst" / "NOTICE")
    licenses = package / "inst" / "licenses"
    shutil.rmtree(licenses, ignore_errors=True)
    shutil.copytree(ROOT / "licenses", licenses)


def fingerprints(root: Path) -> dict[str, tuple[str, str]]:
    if not root.exists() and not root.is_symlink():
        return {}
    paths = [root] if not root.is_dir() else sorted(root.rglob("*"))
    result = {}
    for path in paths:
        if path.is_symlink():
            value = ("link", os.readlink(path))
        elif path.is_file():
            with path.open("rb") as stream:
                digest = hashlib.file_digest(stream, "sha256").hexdigest()
            value = ("file", digest)
        else:
            continue
        name = "." if path == root else path.relative_to(root).as_posix()
        result[name] = value
    return result


def check() -> bool:
    with tempfile.TemporaryDirectory(prefix="rumi-r-vendor-") as directory:
        expected = Path(directory) / "r"
        sync(expected)
        stale = []
        for managed in MANAGED:
            wanted = fingerprints(expected / managed)
            current = fingerprints(PACKAGE / managed)
            for name in sorted(wanted.keys() | current.keys()):
                label = (
                    (managed / name).as_posix() if name != "." else managed.as_posix()
                )
                if name not in current:
                    stale.append(f"missing {label}")
                elif name not in wanted:
                    stale.append(f"extra {label}")
                elif current[name] != wanted[name]:
                    stale.append(f"changed {label}")

    if stale:
        print("R vendor is out of date; run make r-vendor")
        for item in stale[:50]:
            print(f"  {item}")
        if len(stale) > 50:
            print(f"  ... and {len(stale) - 50} more")
        return False
    print("R vendor is current")
    return True


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()

    if not (OPENZL / "deps" / "zstd" / "lib" / "zstd.h").exists():
        raise SystemExit("OpenZL and its dependencies are missing; run make submodules")
    if args.check:
        raise SystemExit(0 if check() else 1)
    sync(PACKAGE)


if __name__ == "__main__":
    main()
