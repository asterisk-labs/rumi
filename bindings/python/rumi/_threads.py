import operator
import warnings

from ._ffi import lib


def _apply(n: int) -> int:
    effective = lib.rumi_set_num_threads(n)
    if effective != n:
        warnings.warn(
            f"rumi's thread count is pinned at {effective}; the request for "
            f"{n} was ignored. Configure it before the first parallel read.",
            RuntimeWarning, stacklevel=3)
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
    return _apply(_validate(n))


def get_num_threads() -> int:
    """Return the process-wide count currently configured or pinned."""
    return lib.rumi_get_num_threads()
