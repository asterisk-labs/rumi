import numpy as np

from ._repr import _human, frame_html, frame_text

_HEAD, _HEAD_TAIL = 5, 10  # rows either side of the gap, and when to cut
_MAX_CELLS = 24            # past this the drawn face bins, one cell per block
_COLS = ("tile", "cell", "data", "compressed")
_DIMS = ("band", "row", "col")  # indexable, but tile and cell already say them

# What a column name may not be, since attach() puts one on Tile.
_RESERVED = frozenset(_COLS) | frozenset(_DIMS) | {"index", "shape"}


class Tile:
    """One row. A view on its frame, so assigning compressed writes through."""

    __slots__ = ("_f", "_i")

    def __init__(self, frame, i):
        self._f = frame
        self._i = i

    @property
    def index(self):
        return self._i

    @property
    def band(self):
        return int(self._f._band[self._i])

    @property
    def row(self):
        return int(self._f._row[self._i])

    @property
    def col(self):
        return int(self._f._col[self._i])

    @property
    def tile(self):
        return f"{self.band}.{self.row}.{self.col}"

    @property
    def cell(self):
        return f"{self.row}.{self.col}"

    @property
    def data(self):
        return self._f._data[self._i]

    @property
    def shape(self):
        return self._f._data[self._i].shape

    @property
    def compressed(self):
        return self._f._comp[self._i]

    @compressed.setter
    def compressed(self, frame):
        buf = None if frame is None else _frame_bytes(frame, self._i)
        self._f._comp[self._i] = buf
        self._f._size[self._i] = -1 if buf is None else len(buf)

    def __getattr__(self, name):
        try:
            col = self._f._extra[name]
        except KeyError:
            raise AttributeError(
                f"no attribute or attached column {name!r}") from None
        return col[self._i]

    def __repr__(self):
        state = ("pending" if self.compressed is None
                 else _human(len(self.compressed)))
        h, w = self.data.shape
        return (f"<rumi.Tile {self.tile} {w}\u00d7{h} {self.data.dtype} "
                f"{state}>")


class TileFrame:
    """Fixed-grid tiles in (row, col, band) order, band innermost.

    The order is the wire order, so the storage is never sorted or filtered in
    place. Indexing returns Tile views that write back by index.

    Edge tiles are cut to where the image reaches, never padded. Each one goes
    to geozl as its own raster, and geozl reads its width off the last axis, so
    a tile of 512 by 44 needs nothing said about it.
    """

    __slots__ = ("_data", "_comp", "_size", "_band", "_row", "_col", "_extra",
                 "image_width", "image_length", "tile_size", "bands", "dtype")

    columns = _COLS
    dims = _DIMS

    def __init__(self, data, *, image_width, image_length, tile_size, bands,
                 dtype):
        self.image_width = int(image_width)
        self.image_length = int(image_length)
        self.tile_size = t = int(tile_size)
        self.bands = int(bands)
        self.dtype = np.dtype(dtype)
        if t < 16 or t % 16:
            raise ValueError(f"tile_size must be a multiple of 16, got {t}")

        self._data = list(data)
        n = self.tiles_across * self.tiles_down * self.bands
        if len(self._data) != n:
            raise ValueError(
                f"expected {n} tiles for this grid, got {len(self._data)}")

        i = np.arange(n)
        pos, self._band = np.divmod(i, self.bands)
        self._row, self._col = np.divmod(pos, self.tiles_across)

        # Every tile is checked against the shape its grid position implies,
        # which also says it is the tile it claims to be, not merely that they
        # all measure the same.
        for k, a in enumerate(self._data):
            want = self._expected_shape(int(self._row[k]), int(self._col[k]))
            if a.shape != want:
                raise ValueError(
                    f"tile {k} at row {self._row[k]} col {self._col[k]} is "
                    f"{a.shape}, the grid says {want}")
            if a.dtype != self.dtype:
                raise ValueError(
                    f"tile {k} is {a.dtype}, the frame is {self.dtype}")

        self._comp = [None] * n
        self._size = np.full(n, -1, np.int64)
        self._extra = {}

    @classmethod
    def from_array(cls, arr, tile_size=512):
        """Cut a (B, Y, X) cube into tiles, the edges cut short.

        A tile is only as big as the image reaches, so nothing is invented and
        nothing is decoded that was never written.
        """
        arr = np.asarray(arr)
        if arr.ndim != 3:
            raise ValueError(f"expected (B, Y, X), got shape {arr.shape}")
        t = int(tile_size)
        if t < 16 or t % 16:
            raise ValueError(
                f"tile_size must be a multiple of 16, got {tile_size}")

        b, y, x = arr.shape
        down, across = -(-y // t), -(-x // t)
        # Wire order is (row, col, band), band innermost. ascontiguousarray
        # gives each tile a tight buffer, which is what geozl compresses.
        tiles = [
            np.ascontiguousarray(arr[k, ty * t:min(ty * t + t, y),
                                        tx * t:min(tx * t + t, x)])
            for ty in range(down)
            for tx in range(across)
            for k in range(b)
        ]
        return cls(tiles, image_width=x, image_length=y, tile_size=t, bands=b,
                   dtype=arr.dtype)

    def _expected_shape(self, row, col):
        t = self.tile_size
        return (min(t, self.image_length - row * t),
                min(t, self.image_width - col * t))

    @property
    def tiles_across(self):
        return -(-self.image_width // self.tile_size)

    @property
    def tiles_down(self):
        return -(-self.image_length // self.tile_size)

    @property
    def cells(self):
        """The grid positions, one per (row, col), all bands together."""
        return [f"{r}.{c}" for r in range(self.tiles_down)
                for c in range(self.tiles_across)]

    @property
    def done(self):
        return int((self._size >= 0).sum())

    @property
    def nbytes(self):
        return int(self._size[self._size >= 0].sum())

    def attach(self, name, values):
        """Hang a column on the frame, one value per tile or one per cell.

        Scaffolding for the loop that routes tiles to graphs. Never written to
        the file.
        """
        if not isinstance(name, str) or not name.isidentifier():
            raise ValueError(f"name must be an identifier, got {name!r}")
        if name in _RESERVED or hasattr(Tile, name):
            raise ValueError(f"{name!r} is already a Tile attribute")
        n, cells = len(self._data), self.tiles_across * self.tiles_down
        values = list(values)
        if len(values) == n:
            col = values
        elif len(values) == cells:
            col = [values[i // self.bands] for i in range(n)]
        else:
            raise ValueError(
                f"expected {n} values, one per tile, or {cells}, one per cell, "
                f"got {len(values)}")
        self._extra[name] = col
        return self

    def __len__(self):
        return len(self._data)

    def __iter__(self):
        return (Tile(self, i) for i in range(len(self._data)))

    def __getitem__(self, key):
        if isinstance(key, str):
            if key == "data":
                return list(self._data)
            if key == "compressed":
                return list(self._comp)
            if key in self._extra:
                return list(self._extra[key])
            if key == "tile":
                return [f"{b}.{r}.{c}" for b, r, c
                        in zip(self._band, self._row, self._col, strict=True)]
            if key == "cell":
                return [f"{r}.{c}" for r, c in zip(self._row, self._col, strict=True)]
            col = {"band": self._band, "row": self._row, "col": self._col}
            if key not in col:
                known = (*_COLS, *_DIMS, *self._extra)
                raise KeyError(f"no column {key!r}, columns are {known}")
            return col[key].copy()
        # One tile at a time. A slice or a tuple would have to answer what a
        # sub-frame means, and the grid is what makes a frame a frame.
        if not isinstance(key, (int, np.integer)):
            raise TypeError(
                f"index with an int or a column name, got {type(key).__name__}")
        i = int(key)
        if i < 0:
            i += len(self._data)
        if not 0 <= i < len(self._data):
            raise IndexError(f"tile {key} out of range")
        return Tile(self, i)

    def __setitem__(self, key, values):
        if key != "compressed":
            raise KeyError(f"{key!r} is not assignable, only 'compressed' is")
        n = len(self._data)
        values = list(values)
        if len(values) != n:
            raise ValueError(f"expected {n} frames, got {len(values)}")
        # Every frame is checked before any is stored, so a bad one in the
        # middle does not leave the column half written.
        bufs = [None if v is None else _frame_bytes(v, i)
                for i, v in enumerate(values)]
        self._comp = bufs
        self._size = np.array([-1 if b is None else len(b) for b in bufs],
                              np.int64)

    def to_pandas(self):
        """The scalar columns as a DataFrame, for looking. Without data."""
        import pandas as pd
        cols = {
            "tile": self["tile"], "cell": self["cell"],
            "band": self._band, "row": self._row, "col": self._col,
            "bytes": np.where(self._size < 0, np.nan, self._size),
        }
        cols.update({k: list(v) for k, v in self._extra.items()})
        return pd.DataFrame(cols)

    def _raw(self):
        """Uncompressed bytes of the tiles that have a frame."""
        it = self.dtype.itemsize
        return sum(a.size * it
                   for a, s in zip(self._data, self._size, strict=True)
                   if s >= 0)

    def _facts(self):
        raw, nbytes = self._raw(), self.nbytes
        return {"b": self.bands, "y": self.image_length, "x": self.image_width,
                "t": self.tile_size, "across": self.tiles_across,
                "down": self.tiles_down, "n": len(self._data),
                "done": self.done, "dtype": str(self.dtype), "nbytes": nbytes,
                "ratio": raw / nbytes if nbytes else 0.0}

    def _rows(self):
        """The shown rows as (index, band, row, col, shape, size)."""
        n = len(self._data)
        keep = (range(n) if n <= _HEAD_TAIL + 1 else
                [*range(_HEAD), None, *range(n - (_HEAD_TAIL - _HEAD), n)])
        return [None if i is None else
                (i, int(self._band[i]), int(self._row[i]), int(self._col[i]),
                 self._data[i].shape, int(self._size[i])) for i in keep]

    def __repr__(self):
        return frame_text(self._facts(), self._rows(), _COLS)

    def _repr_html_(self):
        f, rows = self._facts(), self._rows()
        return frame_html(f, rows, self._states(), _COLS,
                          frame_text(f, rows, _COLS))

    def _states(self):
        """Per drawn cell, 0 pending, 1 partial, 2 done, as an (ny, nx) array.

        Bins the grid when it is larger than the face can show, so a block is
        done only when every tile under it is.
        """
        across, down, b = self.tiles_across, self.tiles_down, self.bands
        step = max(1, -(-max(across, down) // _MAX_CELLS))
        ny, nx = -(-down // step), -(-across // step)

        hits = np.zeros((ny * step, nx * step), np.int64)
        hits[:down, :across] = (self._size >= 0).reshape(down, across, b).sum(2)
        total = np.zeros_like(hits)
        total[:down, :across] = b

        hits = hits.reshape(ny, step, nx, step).sum((1, 3))
        total = total.reshape(ny, step, nx, step).sum((1, 3))
        return np.where(hits == total, 2, np.where(hits == 0, 0, 1))


def _frame_bytes(v, i):
    buf = bytes(v)
    # Caught on assignment rather than at write time, so the traceback lands in
    # the loop that produced it. A zero-length payload is not a legal tile.
    if not buf:
        raise ValueError(f"tile {i} got an empty frame")
    return buf


def tile(arr, tile_size=512):
    """Cut a (B, Y, X) cube into a TileFrame. Edge tiles are cut, not padded."""
    return TileFrame.from_array(arr, tile_size)