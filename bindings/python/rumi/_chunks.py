import operator
from collections.abc import Sequence

from ._header import RumiHeader

BandSelection = tuple[int, int] | list[int] | None


def _positive(value, name):
    try:
        value = operator.index(value)
    except TypeError:
        raise TypeError(f"{name} must be an integer") from None
    if value <= 0:
        raise ValueError(f"{name} must be positive, got {value}")
    return value


def _tile_shape(tiles):
    if isinstance(tiles, tuple):
        if len(tiles) != 2:
            raise ValueError("tiles must be an integer or a (rows, columns) pair")
        return (_positive(tiles[0], "tile rows"),
                _positive(tiles[1], "tile columns"))
    size = _positive(tiles, "tiles")
    return size, size


def _bands(selection, total):
    if selection is None:
        return None
    if isinstance(selection, tuple):
        if len(selection) != 2:
            raise ValueError("bands: tuple must be (start, stop)")
        try:
            start, stop = map(operator.index, selection)
        except TypeError:
            raise TypeError("bands: start and stop must be integers") from None
        if not 0 <= start < stop <= total:
            raise ValueError(
                f"bands: range ({start}, {stop}) out of [0, {total}]")
        return start, stop
    if isinstance(selection, list):
        if not selection:
            raise ValueError("bands cannot be empty")
        try:
            indexes = tuple(operator.index(i) for i in selection)
        except TypeError:
            raise TypeError("bands: indices must be integers") from None
        for i in indexes:
            if not 0 <= i < total:
                raise ValueError(f"bands: index {i} out of [0, {total})")
        return indexes
    raise TypeError(
        f"bands: expected tuple or list, got {type(selection).__name__}")


def _groups(total, size, clip):
    return (total + size - 1) // size if clip else total // size


class _Chunks(Sequence):
    __slots__ = ("_bands", "_band_list", "_block_h", "_block_w",
                 "_cols", "_height", "_rows", "_time", "_time_count",
                 "_times", "_width")

    def __init__(self, header, tiles, time, bands, edge):
        header = header if isinstance(header, RumiHeader) else RumiHeader(header)
        if header.frame_unit != "h w":
            raise ValueError(
                "chunks requires an 'h w' frame layout so every read uses "
                f"whole frames; got {header.frame_unit!r}")

        tile_rows, tile_cols = _tile_shape(tiles)
        time = _positive(time, "time")
        if edge not in ("drop", "clip"):
            raise ValueError("edge must be 'drop' or 'clip'")

        facts = header.to_dict()
        tile_w, tile_h = facts["tile"]
        self._height = facts["height"]
        self._width = facts["width"]
        self._block_h = tile_rows * tile_h
        self._block_w = tile_cols * tile_w
        self._time = time
        self._time_count = facts["time_count"]
        self._bands = _bands(bands, facts["bands"])
        self._band_list = isinstance(bands, list)

        clip = edge == "clip"
        self._rows = _groups(self._height, self._block_h, clip)
        self._cols = _groups(self._width, self._block_w, clip)
        self._times = _groups(self._time_count, time, clip)

    def __len__(self):
        return self._rows * self._cols * self._times

    def __repr__(self):
        return f"<rumi.chunks {len(self)} samples>"

    def __getitem__(self, index):
        if isinstance(index, slice):
            return [self[i] for i in range(*index.indices(len(self)))]
        try:
            index = operator.index(index)
        except TypeError:
            raise TypeError("chunk indices must be integers or slices") from None
        if index < 0:
            index += len(self)
        if not 0 <= index < len(self):
            raise IndexError("chunk index out of range")

        spatial, step = divmod(index, self._times)
        row, col = divmod(spatial, self._cols)
        y = row * self._block_h
        x = col * self._block_w
        t = step * self._time

        bands = self._bands
        if self._band_list:
            bands = list(bands)
        return {
            "window": (y, x, min(self._block_h, self._height - y),
                       min(self._block_w, self._width - x)),
            "time": (t, min(t + self._time, self._time_count)),
            "bands": bands,
        }


def chunks(header: bytes | bytearray | memoryview | RumiHeader, *,
           tiles: int | tuple[int, int] = 1, time: int = 1,
           bands: BandSelection = None, edge: str = "drop") -> Sequence[dict]:
    """Plan tile-aligned reads without opening the rumi file.

    ``tiles=2`` is shorthand for ``tiles=(2, 2)``. ``time`` is the number of
    consecutive steps in each sample, and ``bands`` is repeated in every
    sample; ``None`` keeps all bands. ``edge='drop'`` omits incomplete spatial
    or time groups, while ``'clip'`` keeps them.

    Windows run in row-major order, with time varying fastest. Only ``h w``
    frame layouts are accepted, where a frame holds one spatial tile for one
    band and one time step.
    """
    return _Chunks(header, tiles, time, bands, edge)
