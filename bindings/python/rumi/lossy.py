import struct

import numpy as np
from openzl import ext as _ext

from ._ffi import ffi, lib


_QUANT_LINEAR_CTID = 0x72D780

# Mirrors the ql_dtype enum in graph_quant_linear.h. Order is the wire value.
_QL_DTYPE = {
    np.dtype("uint8"): 0, np.dtype("uint16"): 1, np.dtype("uint32"): 2,
    np.dtype("uint64"): 3, np.dtype("int8"): 4, np.dtype("int16"): 5,
    np.dtype("int32"): 6, np.dtype("int64"): 7, np.dtype("float16"): 8,
    np.dtype("float32"): 9, np.dtype("float64"): 10,
}

_HEADER = struct.Struct("<Bd")          # dtype byte, scale as f64
_FV = lib.rumi_openzl_format_version()
_CHECKSUM_DISABLE = 2                    # OpenZL ternary param, disable


def _ptr(arr):
    return ffi.cast("void *", arr.ctypes.data)


_desc = _ext.MultiInputCodecDescription(
    id=_QUANT_LINEAR_CTID,
    name="rumi.lossy.quant_linear",
    input_types=[_ext.Type.Numeric],
    singleton_output_types=[_ext.Type.Numeric],
)


class _QuantLinearEncoder(_ext.CustomEncoder):
    def __init__(self, scale, dtype):
        super().__init__()              # load bearing, inits the C++ side
        self._scale = scale
        self._dtype = dtype

    def multi_input_description(self):
        return _desc

    def encode(self, state):
        inp = state.inputs[0]
        n, elt = inp.num_elts, inp.elt_width
        state.send_codec_header(_HEADER.pack(self._dtype, self._scale))
        out = state.create_output(0, n, elt)
        lib.rumi_quant_linear_encode(
            _ptr(out.mut_content.as_nparray()),
            _ptr(inp.content.as_nparray()),
            self._scale, self._dtype, n)
        out.commit(n)


class _QuantLinearDecoder(_ext.CustomDecoder):
    def multi_input_description(self):
        return _desc

    def decode(self, state):
        inp = state.singleton_inputs[0]
        n, elt = inp.num_elts, inp.elt_width
        dtype, scale = _HEADER.unpack(bytes(state.codec_header))
        out = state.create_output(0, n, elt)
        lib.rumi_quant_linear_decode(
            _ptr(out.mut_content.as_nparray()),
            _ptr(inp.content.as_nparray()),
            scale, dtype, n)
        out.commit(n)


def quant_linear(max_error, dtype):
    """Head of graph lossy node. Quantizes to a uniform step of 2*max_error,
    bounding the per pixel error by max_error. Omit it for lossless."""
    qd = _QL_DTYPE.get(np.dtype(dtype))
    if qd is None:
        raise ValueError(f"quant_linear does not support dtype {dtype!r}")
    scale = 2.0 * float(max_error)
    if scale <= 0.0:
        raise ValueError("max_error must be positive")

    def build(compressor, successor):
        if not isinstance(successor, _ext.GraphID):
            successor = successor.parameterize(compressor)
        node = compressor.register_custom_encoder(_QuantLinearEncoder(scale, qd))
        return compressor.build_static_graph(
            node, [successor], name="rumi.lossy.quant_linear")

    return build


def register_decoders(dctx) -> None:
    dctx.register_custom_decoder(_QuantLinearDecoder())


def _compressor(recipe):
    c = _ext.Compressor()
    c.set_parameter(_ext.CParam.FormatVersion, _FV)
    g = recipe[-1]()
    for nf in reversed(recipe[:-1]):
        g = nf()(c, g)
    if not isinstance(g, _ext.GraphID):
        g = g.parameterize(c)
    c.select_starting_graph(g)
    return c


def compress(chunks, recipe, *, content_checksum=False):
    """One OpenZL frame per tile, ready for rumi.write. recipe ends in a terminal
    graph, for example [lambda: quant_linear(5, np.uint16), G.Compress]. Keep
    content_checksum off for any recipe with a lossy node, its content hash
    assumes a bit exact round trip."""
    comp = _compressor(recipe)
    frames = []
    for i in range(len(chunks)):
        cc = _ext.CCtx()
        cc.ref_compressor(comp)
        cc.set_parameter(_ext.CParam.FormatVersion, _FV)
        if not content_checksum:
            cc.set_parameter(_ext.CParam.ContentChecksum, _CHECKSUM_DISABLE)
        tile = np.ascontiguousarray(chunks[i]).reshape(-1)
        frames.append(bytes(cc.compress([_ext.Input(_ext.Type.Numeric, tile)])))
    return frames


__all__ = ["compress", "quant_linear", "register_decoders"]