#!/usr/bin/env python3
"""Validate the release metadata shared by RUMI's build and workflows."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

import tomllib

ROOT = Path(__file__).resolve().parents[1]
SEMVER = re.compile(
    r"^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)"
    r"(?:-[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?"
    r"(?:\+[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?$"
)


def fail(message: str) -> None:
    raise ValueError(message)


def read_version(path: Path, label: str) -> str:
    if not path.is_file():
        fail(f"missing {label}: {path.relative_to(ROOT)}")
    version = path.read_text().strip()
    if not SEMVER.fullmatch(version):
        fail(f"{label} {version!r} is not valid SemVer")
    return version


def next_minor_constraint(version: str) -> str:
    major, minor, _ = version.split(".", 2)
    return f"geozl>={version},<{major}.{int(minor) + 1}"


def r_package_version(version: str, changelog: str) -> str:
    """Map a prerelease to an R version after the latest stable release."""
    base, separator, prerelease = version.partition("-")
    if not separator:
        return base
    match = re.fullmatch(r"(alpha|beta|rc)\.(\d+)", prerelease)
    if match is None:
        fail(f"cannot map prerelease {version!r} to an R version")
    target = tuple(map(int, base.split(".")))
    releases = [
        tuple(map(int, found.split(".")))
        for found in re.findall(
            r"^## \[(\d+\.\d+\.\d+)\] - \d{4}-\d{2}-\d{2}$",
            changelog,
            re.MULTILINE,
        )
    ]
    previous = max((item for item in releases if item < target), default=None)
    if previous is None:
        fail(f"cannot find a stable release before {version}")
    phase = {"alpha": 0, "beta": 100, "rc": 200}[match.group(1)]
    build = 9000 + phase + int(match.group(2))
    return ".".join(map(str, (*previous, build)))


def check(tag: str | None) -> tuple[str, str, str]:
    rumi_version = read_version(ROOT / "VERSION", "RUMI VERSION")
    geozl_version = read_version(ROOT / "extern/geozl/VERSION", "GeoZL VERSION")
    karu_version = read_version(ROOT / "extern/karu/VERSION", "Karu VERSION")

    if tag is not None:
        tag_version = tag.removeprefix("refs/tags/").removeprefix("v")
        if tag_version != rumi_version:
            fail(f"tag v{tag_version} does not match VERSION {rumi_version}")

    changelog = (ROOT / "CHANGELOG.md").read_text()
    heading = re.compile(
        rf"^## \[{re.escape(rumi_version)}\] - \d{{4}}-\d{{2}}-\d{{2}}$",
        re.MULTILINE,
    )
    if not heading.search(changelog):
        fail(f"CHANGELOG.md has no dated [{rumi_version}] release entry")

    pyproject_path = ROOT / "bindings/python/pyproject.toml"
    with pyproject_path.open("rb") as stream:
        pyproject = tomllib.load(stream)
    project = pyproject["project"]
    if project.get("dynamic") != ["version"]:
        fail("Python version must remain dynamic and sourced from VERSION")

    js_package_path = ROOT / "bindings/javascript/package.json"
    try:
        js_package = json.loads(js_package_path.read_text())
    except json.JSONDecodeError as exc:
        fail(f"invalid JavaScript package.json: {exc}")
    if js_package.get("version") != rumi_version:
        fail(
            "JavaScript package version "
            f"{js_package.get('version')!r} does not match VERSION {rumi_version}"
        )
    js_lock_path = ROOT / "bindings/javascript/package-lock.json"
    try:
        js_lock = json.loads(js_lock_path.read_text())
    except json.JSONDecodeError as exc:
        fail(f"invalid JavaScript package-lock.json: {exc}")
    lock_versions = {
        js_lock.get("version"),
        js_lock.get("packages", {}).get("", {}).get("version"),
    }
    if lock_versions != {rumi_version}:
        fail(
            f"JavaScript package-lock versions {lock_versions!r} "
            f"do not match VERSION {rumi_version}"
        )

    r_description = (ROOT / "bindings/r/DESCRIPTION").read_text()
    r_match = re.search(r"^Version:\s*(\S+)\s*$", r_description, re.MULTILINE)
    expected_r_version = r_package_version(rumi_version, changelog)
    if r_match is None or r_match.group(1) != expected_r_version:
        found = None if r_match is None else r_match.group(1)
        fail(
            f"R package version {found!r} does not match "
            f"VERSION {rumi_version} ({expected_r_version})"
        )

    julia_project_path = ROOT / "bindings/julia/Project.toml"
    with julia_project_path.open("rb") as stream:
        julia_project = tomllib.load(stream)
    if julia_project.get("version") != rumi_version:
        fail(
            "Julia package version "
            f"{julia_project.get('version')!r} does not match VERSION {rumi_version}"
        )

    expected = next_minor_constraint(geozl_version)
    if expected not in project.get("dependencies", []):
        fail(f"Python dependencies must contain {expected!r}")

    classifiers = set(project.get("classifiers", []))
    required = {
        "Development Status :: 4 - Beta",
        "Programming Language :: Python :: 3.11",
        "Programming Language :: Python :: 3.12",
        "Programming Language :: Python :: 3.13",
        "Programming Language :: Python :: 3.14",
    }
    missing = sorted(required - classifiers)
    if missing:
        fail(f"missing Python classifiers: {missing}")

    for notice in (
        "LICENSE",
        "NOTICE",
        "licenses/LICENSE.GeoZL",
        "licenses/LICENSE.OpenZL",
        "licenses/LICENSE.Zstandard",
        "licenses/LICENSE.LZ4",
        "licenses/LICENSE.Karu",
        "licenses/LICENSE.curl",
        "licenses/LICENSE.OpenSSL",
    ):
        if not (ROOT / notice).is_file():
            fail(f"missing release notice: {notice}")

    notice = (ROOT / "NOTICE").read_text()
    if f"GeoZL {geozl_version}" not in notice:
        fail(f"NOTICE does not name the pinned GeoZL version {geozl_version}")
    if f"Karu {karu_version}" not in notice:
        fail(f"NOTICE does not name the pinned Karu version {karu_version}")

    return rumi_version, geozl_version, karu_version


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tag", help="release tag or refs/tags/vX.Y.Z")
    parser.add_argument("--print-r-version", action="store_true")
    args = parser.parse_args(argv)
    if args.print_r_version:
        try:
            version = read_version(ROOT / "VERSION", "RUMI VERSION")
            changelog = (ROOT / "CHANGELOG.md").read_text()
            print(r_package_version(version, changelog))
        except (OSError, ValueError) as exc:
            print(f"release metadata error: {exc}", file=sys.stderr)
            return 1
        return 0
    try:
        rumi_version, geozl_version, karu_version = check(args.tag)
    except (KeyError, OSError, tomllib.TOMLDecodeError, ValueError) as exc:
        print(f"release metadata error: {exc}", file=sys.stderr)
        return 1
    print(
        f"release metadata OK: rumi {rumi_version}, "
        f"geozl {geozl_version}, karu {karu_version}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
