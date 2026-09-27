from ._ffi import _check, ffi, lib


def set_checksum_verification(on: bool) -> bool:
    """Set process-wide OpenZL checksum verification.

    Call before the first read. RUMI_VERIFY accepts explicit true and false
    spellings documented by the C API.
    Returns the setting in effect.
    """
    if not isinstance(on, bool):
        raise TypeError(f"checksum verification must be a bool, got {on!r}")
    want = on
    _check(lib.rumi_set_checksum_verification(1 if want else 0))
    return want


def get_checksum_verification() -> bool:
    """Return the process-wide setting currently configured or pinned."""
    out = ffi.new("int*")
    _check(lib.rumi_get_checksum_verification(out))
    return out[0] != 0
