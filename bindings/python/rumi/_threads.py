"""The process-wide thread count, the same shape torch gives it."""

import operator
import warnings

from ._ffi import lib


# stacklevel is passed in so the warning lands on the caller's line whether it
# came through set_num_threads or through read's num_threads.
def _apply(n: int, stacklevel: int) -> int:
    effective = lib.rumi_set_num_threads(n)
    if effective != n:
        warnings.warn(
            f"rumi's thread count is pinned at {effective}, so the request for "
            f"{n} was ignored. Set the count before the first parallel read, "
            f"or through RUMI_NUM_THREADS.",
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
    """Set the thread count reads take when they name none.

    Process-wide, like torch.set_num_threads. Call it before the first read that
    asks for more than one thread: once that read reserves the count, a later
    call warns and changes nothing. RUMI_NUM_THREADS seeds the value, an
    integer or ALL_CPUS. A forked child does not inherit the pinned state; it
    evaluates its environment again and therefore defaults to 1 when unset.

    Returns the count in effect after the call, which is the old one when the
    request was refused.
    """
    return _apply(_validate(n), stacklevel=3)


def get_num_threads() -> int:
    """Return the process-wide count currently configured or pinned."""
    return lib.rumi_get_num_threads()


def resolve(n: int | None) -> int:
    """Turn a read's num_threads into the one the C ABI wants.

    None defers to the process-wide count. 1 stays on the calling thread and
    never builds the pool, so a DataLoader worker asking for it cannot fix the
    size for its whole process. Anything else sets the count, reports a refused
    resize, and passes the effective value explicitly so a concurrent setter
    cannot silently change this read before the C core pins the pool.
    """
    if n is None:
        return 0
    n = _validate(n)
    if n == 1:
        return 1
    return _apply(n, stacklevel=4)
