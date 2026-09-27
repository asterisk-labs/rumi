from dataclasses import dataclass

import numpy as np

from ._ffi import _check, ffi, lib


@dataclass(frozen=True, repr=False)
class DType:
    """A Rumi sample type and its exact decoded representation."""

    code: int
    name: str
    sample_format: int
    bits: int
    itemsize: int
    component_size: int
    dlpack: tuple[int, int, int]
    numpy_dtype: type[np.generic] | None

    def __repr__(self) -> str:
        return f"<rumi.DType {self.name}>"

    def __str__(self) -> str:
        return self.name


def _load_registry() -> dict[int, DType]:
    out = ffi.new("const rumi_dtype_info**")
    count = lib.rumi_dtype_registry(out)
    registry = {}
    for i in range(count):
        row = out[0][i]
        named = (ffi.string(row.numpy).decode("ascii")
                 if row.numpy != ffi.NULL else None)
        numpy_scalar = getattr(np, named) if named is not None else None
        code = int(row.code)
        registry[code] = DType(
            code=code,
            name=ffi.string(row.name).decode("ascii"),
            sample_format=int(row.sample_format),
            bits=int(row.bits),
            itemsize=int(row.storage_bytes),
            component_size=int(row.component_bytes),
            dlpack=(int(row.dl_code), int(row.dl_bits), int(row.dl_lanes)),
            numpy_dtype=numpy_scalar,
        )
    return registry


_DTYPES = _load_registry()


def dtype_info(code: int) -> DType:
    try:
        return _DTYPES[code]
    except KeyError:
        raise KeyError(f"unknown rumi dtype code {code}") from None


def name(code: int) -> str:
    return dtype_info(code).name


def dtype_code(dtype) -> int:
    resolved = np.dtype(dtype)
    for code, info in _DTYPES.items():
        if info.numpy_dtype is resolved.type:
            return code
    raise TypeError(f"dtype {resolved} is not supported by rumi's NumPy writer")


def needs_sample_validation(code: int) -> bool:
    info = dtype_info(code)
    return info.bits < info.itemsize * 8


def check_samples(arr, code: int) -> None:
    """Validate decoded samples using the core's dtype rules."""
    view = arr.reshape(-1).view("uint8")
    _check(lib.rumi_check_samples(ffi.from_buffer(view), view.nbytes,
                                  code))
