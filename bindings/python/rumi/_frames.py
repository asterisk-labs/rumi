import numpy as np

from ._dtype import dtype_code
from ._pattern import AXES, COLUMN, GRID_COLUMNS, compile_pattern, frame_at, frame_count
from ._repr import _human, frame_html, frame_text

_HEAD, _HEAD_TAIL = 5, 10  # rows either side of the gap, and when to cut
_MAX_CELLS = 24            # past this the drawn face bins, one cell per block
_PAYLOAD_COLS = ("data", "compressed")

# What a column name may not be, since attach() puts one on Frame. Every name a
# table can carry at any layout, so the guard does not depend on this one.
_RESERVED = frozenset(
    {"tile", "cell", "index", "shape", "unit", "pattern"}
    | set(_PAYLOAD_COLS) | set(GRID_COLUMNS) | {COLUMN[a] for a in AXES})


class Frame:
    """View of one FrameTable row."""

    __slots__ = ("_f", "_i")

    def __init__(self, frame, i):
        self._f = frame
        self._i = i

    @property
    def index(self):
        return self._i

    @property
    def band(self):
        if self._f._band is None:
            raise AttributeError(
                "a cell frame holds every band, so it has no band of its own")
        return int(self._f._band[self._i])

    @property
    def row(self):
        return int(self._f._row[self._i])

    @property
    def col(self):
        return int(self._f._col[self._i])

    @property
    def tile(self):
        if self._f._band is None:
            raise AttributeError(
                "a cell frame is not one tile, use cell")
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
        shape = "\u00d7".join(str(d) for d in reversed(self.data.shape))
        name = self.tile if self._f._band is not None else self.cell
        return (f"<rumi.Frame {name} {shape} {self.data.dtype} {state}>")


class FrameTable:
    """Frames in wire order, one row per frame.

    The pattern says what a frame holds and how its axes are ordered. Frames
    are always tile-interleaved: the grid is walked row-major, and a band that
    the frame does not hold is the innermost index axis.

    Indexing returns Frame views that update the table.
    """

    __slots__ = ("_data", "_comp", "_size", "_band", "_row", "_col", "_extra",
                 "image_width", "image_length", "tile_size", "bands", "dtype",
                 "pattern")


    def __init__(self, data, *, image_width, image_length, tile_size, bands,
                 dtype, pattern):
        self.pattern = (pattern if not isinstance(pattern, str)
                        else compile_pattern(pattern))
        self.image_width = int(image_width)
        self.image_length = int(image_length)
        self.tile_size = t = int(tile_size)
        self.bands = int(bands)
        self.dtype = np.dtype(dtype)
        dtype_code(self.dtype)
        if self.image_width <= 0 or self.image_length <= 0:
            raise ValueError(
                f"image dimensions must be positive, got "
                f"{self.image_width}x{self.image_length}"
            )
        if self.bands <= 0:
            raise ValueError(f"bands must be positive, got {self.bands}")
        if not 1 <= t <= 65535:
            raise ValueError(f"tile_size must be in [1, 65535], got {t}")

        self._data = list(data)
        n = self._geometry()[2]
        if len(self._data) != n:
            raise ValueError(
                f"expected {n} ({' '.join(self.pattern.frame_axes)}) frames "
                f"for this grid, got {len(self._data)}")

        indexed = self.pattern.bands_are_indexed
        self._row = np.empty(n, np.int64)
        self._col = np.empty(n, np.int64)
        self._band = np.empty(n, np.int64) if indexed else None

        # Every frame is checked against the shape the core says its position
        # implies, which also says it is the frame it claims to be, not merely
        # that they all measure the same.
        for k, a in enumerate(self._data):
            at = self._at(k)
            self._row[k], self._col[k] = at.row, at.col
            if indexed:
                self._band[k] = at.band
            if a.shape != at.dims:
                raise ValueError(
                    f"frame {k} at row {at.row} col {at.col} is "
                    f"{a.shape}, the grid says {at.dims}")
            if a.dtype != self.dtype:
                raise ValueError(
                    f"frame {k} is {a.dtype}, the table is {self.dtype}")

        self._comp = [None] * n
        self._size = np.full(n, -1, np.int64)
        self._extra = {}

    @classmethod
    def from_array(cls, arr, pattern, tile_size=512):
        """Cut an array into frames clipped to image bounds.

        The pattern names the input's axes, so it need not arrive as (B, Y, X).
        """
        p = pattern if not isinstance(pattern, str) else compile_pattern(pattern)
        arr = np.asarray(arr)
        if arr.ndim != len(p.input_axes):
            raise ValueError(
                f"the pattern names {len(p.input_axes)} axes "
                f"({' '.join(p.input_axes)}), got shape {arr.shape}")
        t = int(tile_size)
        if not 1 <= t <= 65535:
            raise ValueError(f"tile_size must be in [1, 65535], got {tile_size}")

        # Slice in canonical order whatever order the caller holds. A
        # transpose of the whole array is a view; only the cut copies.
        arr = np.transpose(
            arr, [p.input_axes.index(a) for a in p.canonical_input])
        b, y, x = arr.shape
        canon = p.canonical_axes
        order = [canon.index(a) for a in p.frame_axes]
        indexed = p.bands_are_indexed
        n = frame_count(p.frame_unit, x, y, t, b)[2]

        # The core says where every frame sits and in what order, so the wire
        # order is not rebuilt here. ascontiguousarray gives each frame a tight
        # buffer, which is what geozl compresses.
        data = []
        for k in range(n):
            at = frame_at(p.frame_unit, x, y, t, b, k)
            ys = slice(at.row * t, at.row * t + at.h)
            xs = slice(at.col * t, at.col * t + at.w)
            cut = (arr[at.band, ys, xs] if indexed
                   else arr[:, ys, xs].transpose(order))
            data.append(np.ascontiguousarray(cut))

        return cls(data, image_width=x, image_length=y, tile_size=t, bands=b,
                   dtype=arr.dtype, pattern=p)

    def _geometry(self):
        """Grid and frame count, from the core."""
        return frame_count(self.pattern.frame_unit, self.image_width,
                           self.image_length, self.tile_size, self.bands)

    def _at(self, index):
        """Where one frame sits, from the core."""
        return frame_at(self.pattern.frame_unit, self.image_width,
                        self.image_length, self.tile_size, self.bands, index)

    @property
    def dims(self):
        """Column names for the axes the frame index walks."""
        return self.pattern.index_columns

    @property
    def tiles_across(self):
        return self._geometry()[0]

    @property
    def tiles_down(self):
        return self._geometry()[1]

    @property
    def cells(self):
        """Return grid-position labels in row-major order."""
        return [f"{r}.{c}" for r in range(self.tiles_down)
                for c in range(self.tiles_across)]

    @property
    def done(self):
        return int((self._size >= 0).sum())

    @property
    def nbytes(self):
        return int(self._size[self._size >= 0].sum())

    def attach(self, name, values):
        """Attach values per frame or per grid position.

        Per-position values are expanded across tile frames. Attached columns
        are not written to the rumi file.
        """
        if not isinstance(name, str) or not name.isidentifier():
            raise ValueError(f"name must be an identifier, got {name!r}")
        if name in _RESERVED or hasattr(Frame, name):
            raise ValueError(f"{name!r} is already a Frame attribute")
        n, cells = len(self._data), self.tiles_across * self.tiles_down
        values = list(values)
        if len(values) == n:
            col = values
        elif len(values) == cells:
            col = [values[i // self.bands] for i in range(n)]
        else:
            raise ValueError(
                f"expected {n} values, one per frame, or {cells}, one per "
                f"cell, got {len(values)}")
        self._extra[name] = col
        return self

    def __len__(self):
        return len(self._data)

    def __iter__(self):
        return (Frame(self, i) for i in range(len(self._data)))

    def __getitem__(self, key):
        if isinstance(key, str):
            if key == "data":
                return list(self._data)
            if key == "compressed":
                return list(self._comp)
            if key in self._extra:
                return list(self._extra[key])
            if key == "cell":
                return [f"{r}.{c}" for r, c in zip(self._row, self._col, strict=True)]
            if self._band is not None and key == "tile":
                return [f"{b}.{r}.{c}" for b, r, c
                        in zip(self._band, self._row, self._col, strict=True)]
            col = {"row": self._row, "col": self._col}
            if self._band is not None:
                col["band"] = self._band
            if key not in col:
                known = (*self.columns, *col, *self._extra)
                raise KeyError(f"no column {key!r}, columns are {known}")
            return col[key].copy()
        # Row indexing returns one frame view.
        if not isinstance(key, (int, np.integer)):
            raise TypeError(
                f"index with an int or a column name, got {type(key).__name__}")
        i = int(key)
        if i < 0:
            i += len(self._data)
        if not 0 <= i < len(self._data):
            raise IndexError(f"frame {key} out of range")
        return Frame(self, i)

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
        """Return scalar metadata columns as a DataFrame."""
        import pandas as pd
        cols = {}
        if self._band is not None:
            cols["tile"] = self["tile"]
        cols["cell"] = self["cell"]
        if self._band is not None:
            cols["band"] = self._band
        cols["row"] = self._row
        cols["col"] = self._col
        cols["bytes"] = np.where(self._size < 0, np.nan, self._size)
        cols.update({k: list(v) for k, v in self._extra.items()})
        return pd.DataFrame(cols)

    def _raw(self):
        """Return decoded bytes represented by completed frames."""
        it = self.dtype.itemsize
        return sum(a.size * it
                   for a, s in zip(self._data, self._size, strict=True)
                   if s >= 0)

    def _facts(self):
        raw, nbytes = self._raw(), self.nbytes
        return {"b": self.bands, "y": self.image_length, "x": self.image_width,
                "t": self.tile_size, "across": self.tiles_across,
                "down": self.tiles_down, "n": len(self._data),
                "layout": str(self.pattern),
                "tiled": self.pattern.bands_are_indexed,
                "done": self.done, "dtype": str(self.dtype), "nbytes": nbytes,
                "ratio": raw / nbytes if nbytes else 0.0}

    def _rows(self):
        """Return rows used by the compact representation."""
        n = len(self._data)
        keep = (range(n) if n <= _HEAD_TAIL + 1 else
                [*range(_HEAD), None, *range(n - (_HEAD_TAIL - _HEAD), n)])
        return [None if i is None else
                (i, None if self._band is None else int(self._band[i]),
                 int(self._row[i]), int(self._col[i]),
                 self._data[i].shape, int(self._size[i])) for i in keep]

    @property
    def columns(self):
        """Return the columns shown by the representation."""
        # A frame that does not hold every band needs a label naming which,
        # so the table shows a per-frame column as well as the grid position.
        named = ("tile", "cell") if self.pattern.bands_are_indexed else ("cell",)
        return (*named, *_PAYLOAD_COLS)

    def __repr__(self):
        return frame_text(self._facts(), self._rows(), self.columns)

    def _repr_html_(self):
        f, rows = self._facts(), self._rows()
        return frame_html(f, rows, self._states(), self.columns,
                          frame_text(f, rows, self.columns))

    def _states(self):
        """Return the binned completion state used by the grid preview."""
        across, down = self.tiles_across, self.tiles_down
        per = self.bands if self.pattern.bands_are_indexed else 1
        step = max(1, -(-max(across, down) // _MAX_CELLS))
        ny, nx = -(-down // step), -(-across // step)

        hits = np.zeros((ny * step, nx * step), np.int64)
        hits[:down, :across] = (self._size >= 0).reshape(down, across,
                                                         per).sum(2)
        total = np.zeros_like(hits)
        total[:down, :across] = per

        hits = hits.reshape(ny, step, nx, step).sum((1, 3))
        total = total.reshape(ny, step, nx, step).sum((1, 3))
        return np.where(hits == total, 2, np.where(hits == 0, 0, 1))


def _frame_bytes(v, i):
    if isinstance(v, (int, np.integer)):
        raise TypeError(f"frame {i} must be bytes-like, got {type(v).__name__}")
    buf = bytes(v)
    # Validate on assignment so the traceback identifies the producing loop.
    if not buf:
        raise ValueError(f"frame {i} is empty")
    return buf


def frames(arr, pattern, tile_size=512):
    """Cut an array into a FrameTable.

    The pattern reads ``input -> output``. A parenthesised pair on the left
    splits an axis into a grid axis and a tile-local one; the trailing group on
    the right is the frame.

        "b (row h) (col w) -> row col (b h w)"    every band, band planar
        "b (row h) (col w) -> row col (h w b)"    every band, band chunky
        "b (row h) (col w) -> row col b (h w)"    one band per frame

    Only ``b`` is reserved, so the split names are yours. See ``_pattern`` for
    the grammar and what it refuses.
    """
    return FrameTable.from_array(arr, pattern, tile_size)
