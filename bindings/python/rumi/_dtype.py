import numpy as np

# private sample formats for types TIFF cannot represent
# sub-byte integers use standard integer formats with reduced bit counts
_SF_FLOAT8_E4M3FN = 100
_SF_FLOAT8_E5M2   = 101
_SF_BFLOAT16      = 102
_SF_FLOAT8_E8M0   = 103
_SF_FLOAT6_E2M3   = 104
_SF_FLOAT6_E3M2   = 105
_SF_FLOAT4_E2M1   = 106

_TO_NUMPY: dict[int, type] = {
    1: np.uint8,   2: np.int8,     3: np.uint16,  4: np.int16,
    5: np.uint32,  6: np.int32,    7: np.uint64,  8: np.int64,
    9: np.float16, 10: np.float32, 11: np.float64,
    15: np.complex64, 16: np.complex128,
}

_TO_ENCODING: dict[type, tuple[int, int]] = {
    np.uint8: (1, 8),   np.uint16: (1, 16), np.uint32: (1, 32), np.uint64: (1, 64),
    np.int8: (2, 8),    np.int16: (2, 16),  np.int32: (2, 32),  np.int64: (2, 64),
    np.float16: (3, 16), np.float32: (3, 32), np.float64: (3, 64),
    np.complex64: (6, 64), np.complex128: (6, 128),
}

_NEEDS_ML = frozenset({17, 18, 19, 20, 21, 22, 23, 25, 26, 27, 28})

try:
    import ml_dtypes
    _TO_NUMPY.update({
        17: ml_dtypes.float8_e4m3fn, 18: ml_dtypes.float8_e5m2,
        19: ml_dtypes.bfloat16,      25: ml_dtypes.float8_e8m0fnu,
        20: ml_dtypes.uint4,         21: ml_dtypes.int4,
        22: ml_dtypes.uint2,         23: ml_dtypes.int2,
        26: ml_dtypes.float6_e2m3fn, 27: ml_dtypes.float6_e3m2fn,
        28: ml_dtypes.float4_e2m1fn,
    })
    _TO_ENCODING.update({
        ml_dtypes.float8_e4m3fn: (_SF_FLOAT8_E4M3FN, 8),
        ml_dtypes.float8_e5m2:   (_SF_FLOAT8_E5M2, 8),
        ml_dtypes.bfloat16:      (_SF_BFLOAT16, 16),
        ml_dtypes.float8_e8m0fnu: (_SF_FLOAT8_E8M0, 8),
        ml_dtypes.float6_e2m3fn: (_SF_FLOAT6_E2M3, 6),
        ml_dtypes.float6_e3m2fn: (_SF_FLOAT6_E3M2, 6),
        ml_dtypes.float4_e2m1fn: (_SF_FLOAT4_E2M1, 4),
        ml_dtypes.uint4: (1, 4), ml_dtypes.int4: (2, 4),
        ml_dtypes.uint2: (1, 2), ml_dtypes.int2: (2, 2),
    })
except ImportError:
    pass


def numpy_dtype(rumi_dtype: int) -> type:
    try:
        return _TO_NUMPY[rumi_dtype]
    except KeyError:
        if rumi_dtype == 24:
            raise NotImplementedError(
                "binary is 1-bit, numpy has no scalar for it, read the packed "
                "bytes and popcount") from None
        if rumi_dtype in _NEEDS_ML:
            raise NotImplementedError(
                f"{name(rumi_dtype)} needs ml_dtypes, pip install ml_dtypes"
            ) from None
        raise NotImplementedError(
            f"no numpy view for rumi_dtype {rumi_dtype}") from None


def sample_encoding(dtype) -> tuple[int, int]:
    try:
        return _TO_ENCODING[np.dtype(dtype).type]
    except KeyError:
        raise ValueError(f"unsupported dtype {np.dtype(dtype)}") from None


_NAMES = {
    0: "unknown", 1: "uint8", 2: "int8", 3: "uint16", 4: "int16",
    5: "uint32", 6: "int32", 7: "uint64", 8: "int64",
    9: "float16", 10: "float32", 11: "float64",
    12: "cint16", 13: "cint32", 14: "cfloat16", 15: "cfloat32", 16: "cfloat64",
    17: "float8_e4m3fn", 18: "float8_e5m2", 19: "bfloat16",
    20: "uint4", 21: "int4", 22: "uint2", 23: "int2", 24: "binary",
    25: "float8_e8m0", 26: "float6_e2m3", 27: "float6_e3m2", 28: "float4_e2m1",
}


def name(rumi_dtype: int) -> str:
    return _NAMES.get(rumi_dtype, f"rumi_dtype({rumi_dtype})")