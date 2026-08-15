"""Process-wide read thread configuration."""

import operator
import warnings

from ._ffi import lib


# stacklevel is passed in so the warning lands on the caller's line whether it
# came through set_num_threads or through read's num_threads.
def _apply(n: int, stacklevel: int) -> int:
    effective = lib.rumi_set_num_threads(n)
    if effective != n:
        warnings.warn(
            f"rumi's thread count is pinned at {effective}; the request for "
            f"{n} was ignored. Configure it before the first parallel read.",
            RuntimeWarning, stacklevel=stacklevel)
    return effective


def _validate(n) -> int:
    try:
        n = operator.index(n)
    except TypeError:
        raise TypeError(f"num_threads must be an integer, got {n!r}") from None
    if not 1 <= n <= 1024:
        raise ValueError(f"num_threads must be in [1, 1024], got {n}")
    return n


def set_num_threads(n: int) -> int:
    """Set the process-wide default for reads.

    Call before the first parallel read. RUMI_NUM_THREADS accepts an integer or
    ALL_CPUS. A forked child reads the variable independently and defaults to 1.
    Returns the count in effect.
    """
    return _apply(_validate(n), stacklevel=3)


def get_num_threads() -> int:
    """Return the process-wide count currently configured or pinned."""
    return lib.rumi_get_num_threads()


def resolve(n: int | None) -> int:
    """Validate a read's thread count and apply its process-wide setting."""
    if n is None:
        return 0
    n = _validate(n)
    if n == 1:
        return 1
    return _apply(n, stacklevel=4)
