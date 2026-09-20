import warnings

from ._ffi import lib


def set_checksum_verification(on: bool) -> bool:
    """Set process-wide OpenZL checksum verification.

    Call before the first read. RUMI_VERIFY accepts 1, true, on, or yes.
    Returns the setting in effect.
    """
    want = bool(on)
    effective = lib.rumi_set_checksum_verification(1 if want else 0) != 0
    if effective != want:
        warnings.warn(
            f"rumi's checksum verification is pinned at {effective}; the "
            f"request for {want} was ignored. Configure it before the first "
            f"read.", RuntimeWarning, stacklevel=2)
    return effective


def get_checksum_verification() -> bool:
    """Return the process-wide setting currently configured or pinned."""
    return lib.rumi_get_checksum_verification() != 0
