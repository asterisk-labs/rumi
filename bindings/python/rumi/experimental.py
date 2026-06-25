from ._ffi import ffi, lib

try:
    from openzl import ext as _ext
except ImportError as _exc:  # pragma: no cover
    raise ImportError(
        "rumi.experimental needs the openzl python package, pip install openzl"
    ) from _exc


# CTids in the rumi experimental range. These must match the C++ decoders in
# the reader so a preview frame reads back through rumi.
_DELTA_W_CTID = 0x72D701
_DELTA_N_CTID = 0x72D702
_PLANAR_CTID = 0x72D703

# Element widths the kernels handle, 8 to 64 bit integers.
_SUPPORTED_WIDTHS = (1, 2, 4, 8)


def _ptr(arr):
    return ffi.cast("void *", arr.ctypes.data)


def _make_codec(ctid, name, enc_kernel, dec_kernel):
    desc = _ext.MultiInputCodecDescription(
        id=ctid,
        name=name,
        input_types=[_ext.Type.Numeric],
        singleton_output_types=[_ext.Type.Numeric],
    )

    class Encoder(_ext.CustomEncoder):
        def __init__(self, width):
            super().__init__()
            w = int(width)
            if w < 1:
                raise ValueError(f"{name} width must be positive, got {w}")
            self._width = w

        def multi_input_description(self):
            return desc

        def encode(self, state):
            inp = state.inputs[0]
            n, elt = inp.num_elts, inp.elt_width
            if elt not in _SUPPORTED_WIDTHS:
                raise ValueError(
                    f"{name} element width {elt} not supported, use 1 2 4 or 8"
                )
            # width first then the output, mirroring the C encode binding
            state.send_codec_header(self._width.to_bytes(4, "little"))
            out = state.create_output(0, n, elt)
            enc_kernel(_ptr(out.mut_content.as_nparray()),
                       _ptr(inp.content.as_nparray()),
                       self._width, n, elt)
            out.commit(n)

    class Decoder(_ext.CustomDecoder):
        def multi_input_description(self):
            return desc

        def decode(self, state):
            inp = state.singleton_inputs[0]
            n, elt = inp.num_elts, inp.elt_width
            header = state.codec_header
            if len(header) != 4:
                raise ValueError(f"{name} bad codec header size {len(header)}")
            width = int.from_bytes(header, "little")
            out = state.create_output(0, n, elt)
            dec_kernel(_ptr(out.mut_content.as_nparray()),
                       _ptr(inp.content.as_nparray()),
                       width, n, elt)
            out.commit(n)

    short = name.removeprefix("rumi.")
    Encoder.__name__ = Encoder.__qualname__ = f"_{short}_encoder"
    Decoder.__name__ = Decoder.__qualname__ = f"_{short}_decoder"
    return Encoder, Decoder


_DeltaWEncoder, DeltaWDecoder = _make_codec(
    _DELTA_W_CTID, "rumi.delta_w", lib.rumi_delta_w_encode, lib.rumi_delta_w_decode)
_DeltaNEncoder, DeltaNDecoder = _make_codec(
    _DELTA_N_CTID, "rumi.delta_n", lib.rumi_delta_n_encode, lib.rumi_delta_n_decode)
_PlanarEncoder, PlanarDecoder = _make_codec(
    _PLANAR_CTID, "rumi.planar", lib.rumi_planar_encode, lib.rumi_planar_decode)


class _PredictorNode:
    """Chains like an openzl.ext node. Call it with the compressor and the
    numeric successor graph, get back a GraphID."""

    _encoder = None
    _name = None

    def __init__(self, width):
        self._width = int(width)

    def __call__(self, compressor, successor_graph):
        # build_static_graph wants a GraphID, but the chain may hand us an
        # unparameterized graph object, so resolve it like the built in nodes do
        if not isinstance(successor_graph, _ext.GraphID):
            successor_graph = successor_graph.parameterize(compressor)
        node = compressor.register_custom_encoder(self._encoder(self._width))
        return compressor.build_static_graph(
            node, [successor_graph], name=self._name)


class DeltaWInt(_PredictorNode):
    _encoder = _DeltaWEncoder
    _name = "rumi.delta_w"


class DeltaNInt(_PredictorNode):
    _encoder = _DeltaNEncoder
    _name = "rumi.delta_n"


class PlanarInt(_PredictorNode):
    _encoder = _PlanarEncoder
    _name = "rumi.planar"


def register_decoders(dctx) -> None:
    """Register the three experimental decoders into an openzl.ext DCtx for
    decoding preview frames in pure Python. The rumi reader does this in C."""
    dctx.register_custom_decoder(DeltaWDecoder())
    dctx.register_custom_decoder(DeltaNDecoder())
    dctx.register_custom_decoder(PlanarDecoder())


__all__ = [
    "DeltaNDecoder", "DeltaNInt", "DeltaWDecoder", "DeltaWInt",
    "PlanarDecoder", "PlanarInt", "register_decoders",
]