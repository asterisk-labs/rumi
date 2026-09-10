"""Verify that the editable install loads this checkout and native library."""

import importlib.metadata
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EXPECTED_MODULE = (ROOT / "bindings" / "python" / "rumi" / "__init__.py").resolve()
EXPECTED_VERSION = (ROOT / "VERSION").read_text().strip()


def fail(message: str) -> int:
    print(f"editable install check failed: {message}", file=sys.stderr)
    return 1


def main() -> int:
    try:
        import rumi
    except Exception as exc:
        return fail(f"import rumi raised {exc!r}")

    module_file = getattr(rumi, "__file__", None)
    if module_file is None:
        return fail("import rumi has no module file")
    loaded = Path(module_file).resolve()
    if loaded != EXPECTED_MODULE:
        owners = importlib.metadata.packages_distributions().get("rumi", [])
        provided_by = ", ".join(owners) if owners else "unknown"
        return fail(
            f"import rumi loaded {loaded}, expected {EXPECTED_MODULE}; "
            f"distributions providing that import: {provided_by}. "
            "Remove or deactivate the conflicting distribution and retry"
        )

    if rumi.__version__ != EXPECTED_VERSION:
        return fail(
            f"package metadata is {rumi.__version__}, expected {EXPECTED_VERSION}"
        )

    from rumi._ffi import ffi, lib

    native_version = ffi.string(lib.rumi_version_string()).decode("ascii")
    if native_version != EXPECTED_VERSION:
        return fail(
            f"native library is {native_version}, expected {EXPECTED_VERSION}"
        )

    print(f"rumi {rumi.__version__} ({loaded})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
