from openzl import ext as _ext

from ._ffi import ffi, lib


# Same CTids the C reader registers, so a frame written here reads back there.
_DELTA_W_CTID = 0x72D701
_DELTA_N_CTID = 0x72D702
_PLANAR_CTID = 0x72D703


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
            self._width = int(width)

        def multi_input_description(self):
            return desc

        def encode(self, state):
            inp = state.inputs[0]
            n, elt = inp.num_elts, inp.elt_width
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
            width = int.from_bytes(state.codec_header, "little")
            out = state.create_output(0, n, elt)
            dec_kernel(_ptr(out.mut_content.as_nparray()),
                       _ptr(inp.content.as_nparray()),
                       width, n, elt)
            out.commit(n)

    short = name.rsplit(".", 1)[-1]
    Encoder.__name__ = Encoder.__qualname__ = f"_{short}_encoder"
    Decoder.__name__ = Decoder.__qualname__ = f"_{short}_decoder"
    return Encoder, Decoder


_DeltaWEncoder, DeltaWDecoder = _make_codec(
    _DELTA_W_CTID, "rumi.experimental.delta_w",
    lib.rumi_delta_w_encode, lib.rumi_delta_w_decode)
_DeltaNEncoder, DeltaNDecoder = _make_codec(
    _DELTA_N_CTID, "rumi.experimental.delta_n",
    lib.rumi_delta_n_encode, lib.rumi_delta_n_decode)
_PlanarEncoder, PlanarDecoder = _make_codec(
    _PLANAR_CTID, "rumi.experimental.planar",
    lib.rumi_planar_encode, lib.rumi_planar_decode)


class _PredictorNode:
    """Chains like an openzl.ext node: call with the compressor and the
    successor graph, get back a GraphID."""

    _encoder = None
    _name = None

    def __init__(self, width):
        self._width = int(width)

    def __call__(self, compressor, successor_graph):
        if not isinstance(successor_graph, _ext.GraphID):
            successor_graph = successor_graph.parameterize(compressor)
        node = compressor.register_custom_encoder(self._encoder(self._width))
        return compressor.build_static_graph(
            node, [successor_graph], name=self._name)


class DeltaWInt(_PredictorNode):
    _encoder = _DeltaWEncoder
    _name = "rumi.experimental.delta_w"


class DeltaNInt(_PredictorNode):
    _encoder = _DeltaNEncoder
    _name = "rumi.experimental.delta_n"


class PlanarInt(_PredictorNode):
    _encoder = _PlanarEncoder
    _name = "rumi.experimental.planar"


def register_decoders(dctx) -> None:
    """Register the experimental decoders into an openzl.ext DCtx, for decoding
    preview frames in pure Python. The rumi reader does this in C."""
    dctx.register_custom_decoder(DeltaWDecoder())
    dctx.register_custom_decoder(DeltaNDecoder())
    dctx.register_custom_decoder(PlanarDecoder())


__all__ = [
    "DeltaNDecoder", "DeltaNInt", "DeltaWDecoder", "DeltaWInt",
    "PlanarDecoder", "PlanarInt", "register_decoders",
]