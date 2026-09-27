import operator

from ._ffi import _check, ffi, lib


def set_num_threads(n: int) -> int:
    """Set the process-wide default for reads.

    Call before the first parallel read. RUMI_NUM_THREADS accepts an integer or
    ALL_CPUS. A forked child reads the variable independently and defaults to 1.
    Returns the count in effect.
    """
    try:
        n = operator.index(n)
    except TypeError:
        raise TypeError(f"num_threads must be an integer, got {n!r}") from None
    if not 1 <= n <= 1024:
        raise ValueError(f"num_threads must be in [1, 1024], got {n}")
    _check(lib.rumi_set_num_threads(n))
    return n


def get_num_threads() -> int:
    """Return the process-wide count currently configured or pinned."""
    out = ffi.new("int*")
    _check(lib.rumi_get_num_threads(out))
    return out[0]


def set_checksum_verification(on: bool) -> bool:
    """Set process-wide OpenZL checksum verification.

    Call before the first read. RUMI_VERIFY accepts explicit true and false
    spellings documented by the C API.
    Returns the setting in effect.
    """
    if not isinstance(on, bool):
        raise TypeError(f"checksum verification must be a bool, got {on!r}")
    _check(lib.rumi_set_checksum_verification(1 if on else 0))
    return on


def get_checksum_verification() -> bool:
    """Return the process-wide setting currently configured or pinned."""
    out = ffi.new("int*")
    _check(lib.rumi_get_checksum_verification(out))
    return out[0] != 0
