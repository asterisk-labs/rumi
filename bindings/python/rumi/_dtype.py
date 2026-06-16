import numpy as np

# (sample_format, bits_per_sample) is the canonical sample encoding from the
# spec. Complex int (5) and 16-bit complex float have no numpy equivalent.
_ENCODING: dict[type[np.generic], tuple[int, int]] = {
    np.uint8:      (1, 8),
    np.uint16:     (1, 16),
    np.uint32:     (1, 32),
    np.uint64:     (1, 64),
    np.int8:       (2, 8),
    np.int16:      (2, 16),
    np.int32:      (2, 32),
    np.int64:      (2, 64),
    np.float16:    (3, 16),
    np.float32:    (3, 32),
    np.float64:    (3, 64),
    np.complex64:  (6, 64),
    np.complex128: (6, 128),
}

_DECODE: dict[tuple[int, int], type[np.generic]] = {
    enc: dt for dt, enc in _ENCODING.items()
}


def sample_encoding(dtype) -> tuple[int, int]:
    try:
        return _ENCODING[np.dtype(dtype).type]
    except KeyError:
        raise ValueError(f"unsupported dtype {np.dtype(dtype)}") from None


def np_dtype(sf: int, bps: int) -> type[np.generic]:
    try:
        return _DECODE[(sf, bps)]
    except KeyError:
        raise NotImplementedError(
            f"no numpy dtype for (sample_format={sf}, bits_per_sample={bps})"
        ) from None