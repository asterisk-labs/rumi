from ._assemble import assemble, tile
from ._ffi import lib
from ._read import index_file, parse

_OPENZL_VERSION = lib.rumi_openzl_format_version()
_CHECKSUM_DISABLE = 2   # OpenZL ternary param, quant_linear requires it off

_ALL = ("delta_w", "delta_n", "planar", "med", "average", "wp_static", "delta_1d")

# A preset is the set of predictors its per-tile sweep tries. A single-predictor
# preset is the degenerate sweep, one candidate and no contest, so a fixed-codec
# fast write is a set of size one rather than a separate code path.
_SETS = {
    "2d-simd": ["delta_w", "planar", "wp_static", "delta_1d"],
    "2d-full": ["delta_w", "planar", "wp_static", "delta_1d",
                "delta_n", "med", "average"],
}
_SETS.update({f"2d-{name}": [name] for name in _ALL})


def _builders():
    """Name to graph builder for every predictor. openzl.ext and geozl are
    imported here, not at module load, so reading a rumi file needs neither. A
    builder takes (compressor, terminal, width) and prepends its stage to the
    terminal. geozl residuals are wraparound, so a Zigzag remaps them for the
    entropy stage; OpenZL's own DeltaInt handles sign itself and takes none."""
    import geozl
    import openzl.ext as zl

    p = geozl.lossless

    def spatial(cls):
        return lambda c, term, w: cls(w)(c, zl.nodes.Zigzag()(c, term))

    return {
        "delta_w":   spatial(p.DeltaW),
        "delta_n":   spatial(p.DeltaN),
        "planar":    spatial(p.Planar),
        "med":       spatial(p.Med),
        "average":   spatial(p.Average),
        "wp_static": spatial(p.WpStatic),
        "delta_1d":  lambda c, term, w: zl.nodes.DeltaInt()(c, term),
    }


def _frame(zl, geozl, chunk, width, build, max_error, dtype):
    """One frame from one predictor. predictor -> (zigzag) -> entropy, with
    quant_linear ahead of the predictor when max_error is set."""
    c = zl.Compressor()
    g = build(c, zl.graphs.Entropy()(c), width)
    if max_error is not None:
        g = geozl.lossy.QuantLinear(max_error, dtype)(c, g)
    c.select_starting_graph(g)

    cc = zl.CCtx()
    cc.ref_compressor(c)
    cc.set_parameter(zl.CParam.FormatVersion, _OPENZL_VERSION)
    if max_error is not None:
        cc.set_parameter(zl.CParam.ContentChecksum, _CHECKSUM_DISABLE)
    return bytes(cc.compress([zl.Input(zl.Type.Numeric, chunk.reshape(-1))]))


def _sweep(zl, geozl, chunk, width, names, builders, max_error, dtype):
    """{name: frame} for one chunk over the named predictors."""
    return {name: _frame(zl, geozl, chunk, width, builders[name], max_error, dtype)
            for name in names}


def write(path, arr, *, method="2d-full", chunk_size=512, max_error=None):
    """Tile arr, compress each tile with the method's predictors keeping the
    smallest frame, and assemble the rumi file. max_error activates lossy
    quant_linear ahead of the predictor. Returns (path, header)."""
    import geozl
    import openzl.ext as zl

    if method not in _SETS:
        raise ValueError(f"unknown method {method!r}, one of {sorted(_SETS)}")
    names = _SETS[method]
    builders = _builders()

    chunks, layout = tile(arr, chunk_size)
    frames = [
        min(_sweep(zl, geozl, ch, ch.shape[1], names, builders,
                   max_error, arr.dtype).values(), key=len)
        for ch in chunks
    ]
    assemble(path, frames, layout)
    return path, parse(index_file(path))


def probe(arr, *, method="2d-full", chunk_size=512, max_error=None):
    """Run the sweep without assembling, to choose a method for a dataset. For
    each predictor in the method reports how many tiles it won and the total
    size if it were forced on every tile, plus the adaptive total, keeping the
    smallest per tile. The gap between a forced predictor and adaptive is what a
    fixed-codec method would cost on this data."""
    import geozl
    import openzl.ext as zl

    if method not in _SETS:
        raise ValueError(f"unknown method {method!r}, one of {sorted(_SETS)}")
    names = _SETS[method]
    builders = _builders()

    chunks, _ = tile(arr, chunk_size)
    forced = dict.fromkeys(names, 0)
    wins = dict.fromkeys(names, 0)
    adaptive = 0
    for ch in chunks:
        got = _sweep(zl, geozl, ch, ch.shape[1], names, builders,
                     max_error, arr.dtype)
        best = min(got, key=lambda n: len(got[n]))
        wins[best] += 1
        adaptive += len(got[best])
        for name, frame in got.items():
            forced[name] += len(frame)
    return {"adaptive": adaptive, "forced": forced, "wins": wins}