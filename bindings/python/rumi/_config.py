import operator

from ._ffi import _check, ffi, lib


def set_num_threads(n: int) -> int:
    """Set the process-wide default for reads.

    ``n`` is an integer from 1 to 1024. Set it before the first parallel read;
    once the process-wide pool exists, changing its size raises
    ``RuntimeError``.

    ``RUMI_NUM_THREADS`` provides the initial value and also accepts
    ``ALL_CPUS``. A forked child reads the variable independently and defaults
    to one thread.

    Returns ``n`` after the setting is accepted.
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
    """Return the process-wide read thread count.

    The result is the configured count before the pool exists and its fixed
    size afterwards. An invalid ``RUMI_NUM_THREADS`` value raises
    ``ValueError``.
    """
    out = ffi.new("int*")
    _check(lib.rumi_get_num_threads(out))
    return out[0]


def set_checksum_verification(on: bool) -> bool:
    """Set process-wide OpenZL checksum verification.

    ``on`` must be ``bool``. Set it before the first decoded frame; afterwards,
    changing the process-wide setting raises ``RuntimeError``.

    ``RUMI_VERIFY`` provides the initial value and accepts ``0``/``1``,
    ``false``/``true``, ``off``/``on`` and ``no``/``yes``.

    Returns ``on`` after the setting is accepted.
    """
    if not isinstance(on, bool):
        raise TypeError(f"checksum verification must be a bool, got {on!r}")
    _check(lib.rumi_set_checksum_verification(1 if on else 0))
    return on


def get_checksum_verification() -> bool:
    """Return whether OpenZL checksum verification is enabled.

    The result is the configured value before the first decoded frame and its
    fixed value afterwards. An invalid ``RUMI_VERIFY`` value raises
    ``ValueError``.
    """
    out = ffi.new("int*")
    _check(lib.rumi_get_checksum_verification(out))
    return out[0] != 0
