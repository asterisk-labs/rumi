import numpy as np

from ._ffi import ffi, lib

# DLPack (code, bits) -> Python scalar. The only type knowledge kept in Python,
# it's the DLPack<->numpy binding, not rumi's type set. Codes are frozen by DLPack.
_DL_INT, _DL_UINT, _DL_FLOAT, _DL_BFLOAT, _DL_COMPLEX = 0, 1, 2, 4, 5
_DL_F8_E4M3FN, _DL_F8_E5M2, _DL_F8_E8M0 = 10, 12, 14
_DL_F6_E2M3, _DL_F6_E3M2, _DL_F4_E2M1 = 15, 16, 17

_DL_TO_NP: dict[tuple[int, int], type] = {
    (_DL_UINT, 8): np.uint8,   (_DL_INT, 8): np.int8,
    (_DL_UINT, 16): np.uint16, (_DL_INT, 16): np.int16,
    (_DL_UINT, 32): np.uint32, (_DL_INT, 32): np.int32,
    (_DL_UINT, 64): np.uint64, (_DL_INT, 64): np.int64,
    (_DL_FLOAT, 16): np.float16, (_DL_FLOAT, 32): np.float32,
    (_DL_FLOAT, 64): np.float64,
    (_DL_COMPLEX, 64): np.complex64, (_DL_COMPLEX, 128): np.complex128,
}

# ML types that live in ml_dtypes, not numpy. Keyed even when the package is
# absent so a type can be reported as "needs ml_dtypes" rather than unknown.
_DL_TO_ML_NAME: dict[tuple[int, int], str] = {
    (_DL_F8_E4M3FN, 8): "float8_e4m3fn", (_DL_F8_E5M2, 8): "float8_e5m2",
    (_DL_BFLOAT, 16): "bfloat16",        (_DL_F8_E8M0, 8): "float8_e8m0fnu",
    (_DL_UINT, 4): "uint4", (_DL_INT, 4): "int4",
    (_DL_UINT, 2): "uint2", (_DL_INT, 2): "int2",
    (_DL_F6_E2M3, 6): "float6_e2m3fn", (_DL_F6_E3M2, 6): "float6_e3m2fn",
    (_DL_F4_E2M1, 4): "float4_e2m1fn",
}

# fold the ML scalars into the main map when the package is present
try:
    import ml_dtypes
    for _key, _attr in _DL_TO_ML_NAME.items():
        _DL_TO_NP[_key] = getattr(ml_dtypes, _attr)
except ImportError:
    pass


_NAMES: dict[int, str] = {0: "unknown"}
_TO_NUMPY: dict[int, type] = {}
_TO_ENCODING: dict[type, tuple[int, int]] = {}
_TO_CODE: dict[type, int] = {}
_NEEDS_ML: set[int] = set()


# The type set, names and on-disk encodings come from the C table. We only map
# each row to its Python scalar; a row with no scalar (binary, the cints, or an
# ML type without ml_dtypes) is left out of _TO_NUMPY on purpose.
def _load_table() -> None:
    out = ffi.new("const rumi_dtype_info**")
    n = lib.rumi_dtype_table(out)
    rows = out[0]
    for i in range(n):
        r = rows[i]
        code = int(r.code)
        _NAMES[code] = ffi.string(r.name).decode("ascii")
        key = (int(r.dl_code), int(r.dl_bits))
        np_type = _DL_TO_NP.get(key)
        if np_type is not None:
            _TO_NUMPY[code] = np_type
            _TO_ENCODING[np_type] = (int(r.sample_format), int(r.bits))
            _TO_CODE[np_type] = code
        elif key in _DL_TO_ML_NAME:
            _NEEDS_ML.add(code)


_load_table()


def numpy_dtype(rumi_dtype: int) -> type:
    if rumi_dtype in _TO_NUMPY:
        return _TO_NUMPY[rumi_dtype]
    # only missing case a caller hits in practice: an ML type, no ml_dtypes
    if rumi_dtype in _NEEDS_ML:
        raise NotImplementedError(
            f"{name(rumi_dtype)} needs ml_dtypes, pip install ml_dtypes")
    raise KeyError(rumi_dtype)


def sample_encoding(dtype) -> tuple[int, int]:
    return _TO_ENCODING[np.dtype(dtype).type]


def dtype_code(dtype) -> int:
    return _TO_CODE[np.dtype(dtype).type]


def name(rumi_dtype: int) -> str:
    return _NAMES.get(rumi_dtype, f"rumi_dtype({rumi_dtype})")