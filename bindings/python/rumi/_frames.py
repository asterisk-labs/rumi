import numpy as np

from ._dtype import check_samples, dtype_code, is_subbyte
from ._pattern import (
    AXES,
    COLUMN,
    GRID_COLUMNS,
    compile_pattern,
    frame_at,
    frame_count,
    layout_name,
)
from ._repr import _human, frame_html, frame_text

_HEAD, _HEAD_TAIL = 5, 10  # rows either side of the gap, and when to cut
_MAX_CELLS = 24            # past this the drawn face bins, one cell per block
_PAYLOAD_COLS = ("data", "compressed")

# Names reserved by Frame and by every possible FrameTable layout.
_RESERVED = frozenset(
    {"tile", "cell", "index", "shape", "unit", "pattern"}
    | set(_PAYLOAD_COLS) | set(GRID_COLUMNS) | {COLUMN[a] for a in AXES})


class Frame:
    """A live view of one frame in a FrameTable."""

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
                "this frame holds every band and has no separate band index")
        return int(self._f._band[self._i])

    @property
    def time(self):
        if self._f._time is None:
            raise AttributeError(
                "this frame holds every time step and has no separate time index")
        return int(self._f._time[self._i])

    @property
    def row(self):
        return int(self._f._row[self._i])

    @property
    def col(self):
        return int(self._f._col[self._i])

    @property
    def tile(self):
        if not self._f._walked:
            raise AttributeError(
                "this frame holds every axis and is identified by its cell; use 'cell'")
        return self._f._label(self._i)

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
        # Keep the specific error raised by properties such as band and tile.
        prop = getattr(type(self), name, None)
        if isinstance(prop, property) and prop.fget is not None:
            return prop.fget(self)
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
        name = self.tile if self._f._walked else self.cell
        return (f"<rumi.Frame {name} {shape} {self.data.dtype} {state}>")


class FrameTable:
    """Decoded frames and their compressed payloads, in file order.

    Grid cells appear row by row. The pattern controls which samples each frame
    contains, the order of its axes, and how frames split by band or time are
    ordered.

    Integer indexing returns a live Frame view. Assigning its compressed payload
    updates the table.
    """

    __slots__ = ("_data", "_comp", "_size", "_band", "_time", "_row", "_col",
                 "_extra", "image_width", "image_length", "tile_size", "bands",
                 "time_count", "dtype", "pattern", "frame_unit")

    def __init__(self, data, *, image_width, image_length, tile_size, bands,
                 dtype, pattern, time_count=1):
        self.pattern = (pattern if not isinstance(pattern, str)
                        else compile_pattern(pattern))
        self.image_width = int(image_width)
        self.image_length = int(image_length)
        self.tile_size = t = int(tile_size)
        self.bands = int(bands)
        self.time_count = int(time_count)
        self.dtype = np.dtype(dtype)
        code = dtype_code(self.dtype)
        subbyte = is_subbyte(code)
        if self.image_width <= 0 or self.image_length <= 0:
            raise ValueError(
                f"image dimensions must be positive, got "
                f"{self.image_width}x{self.image_length}"
            )
        if self.bands <= 0:
            raise ValueError(f"bands must be positive, got {self.bands}")
        if self.time_count <= 0:
            raise ValueError(
                f"time_count must be positive, got {self.time_count}")
        if not 1 <= t <= 65535:
            raise ValueError(f"tile_size must be in [1, 65535], got {t}")

        # Singleton band and time axes are omitted from the stored layout.
        self.frame_unit = self.pattern.frame_unit(self.bands, self.time_count)

        self._data = list(data)
        n = self._geometry()[2]
        if len(self._data) != n:
            raise ValueError(
                f"expected {n} ({' '.join(self.pattern.frame_axes)}) frames "
                f"for this grid, got {len(self._data)}")

        walks = self.pattern.index_columns(self.bands, self.time_count)
        self._row = np.empty(n, np.int64)
        self._col = np.empty(n, np.int64)
        self._band = np.empty(n, np.int64) if "band" in walks else None
        self._time = np.empty(n, np.int64) if "time" in walks else None

        # Frames at the image edges may be smaller than tile_size.
        for k, a in enumerate(self._data):
            at = self._at(k)
            self._row[k], self._col[k] = at.row, at.col
            if self._band is not None:
                self._band[k] = at.band
            if self._time is not None:
                self._time[k] = at.time
            if a.shape != at.dims:
                raise ValueError(
                    f"frame {k} at row {at.row} col {at.col} has shape "
                    f"{a.shape}, expected {at.dims}")
            if a.dtype != self.dtype:
                raise ValueError(
                    f"frame {k} has dtype {a.dtype}, expected {self.dtype}")
            if subbyte:
                try:
                    check_samples(a, code)
                except ValueError as exc:
                    raise ValueError(f"frame {k}: {exc}") from None

        self._comp = [None] * n
        self._size = np.full(n, -1, np.int64)
        self._extra = {}

    @classmethod
    def from_array(cls, arr, pattern, tile_size=512):
        """Split an array into frames, clipping those at the image edges.

        The pattern names the input's axes, so it need not arrive as (B, Y, X).
        """
        p = pattern if not isinstance(pattern, str) else compile_pattern(pattern)
        arr = np.asarray(arr)
        if arr.ndim != len(p.input_axes):
            raise ValueError(
                f"the pattern names {len(p.input_axes)} axes "
                f"({' '.join(p.input_axes)}), got shape {arr.shape}")
        tile = int(tile_size)
        if not 1 <= tile <= 65535:
            raise ValueError(f"tile_size must be in [1, 65535], got {tile_size}")

        # Normalize to (b, t, y, x), inserting omitted singleton axes. The
        # transpose is a view; individual frames become contiguous below.
        arr = np.transpose(arr, [p.input_axes.index(a)
                                 for a in p.canonical_input
                                 if a in p.input_axes])
        for i, a in enumerate(p.canonical_input):
            if a not in p.input_axes:
                arr = np.expand_dims(arr, i)
        b, times, y, x = (int(v) for v in arr.shape)

        # Choose the layout after omitting singleton band and time axes.
        unit = p.frame_unit(b, times)
        axes = tuple(layout_name(unit, b, times).split())
        n = frame_count(unit, x, y, tile, b, times)[2]

        # Ask the core which samples belong to each frame.
        data = []
        for k in range(n):
            at = frame_at(unit, x, y, tile, b, times, k)
            ys = slice(at.row * tile, at.row * tile + at.h)
            xs = slice(at.col * tile, at.col * tile + at.w)
            cut = arr[slice(None) if "b" in axes else at.band,
                      slice(None) if "t" in axes else at.time, ys, xs]
            # Apply the frame permutation returned by the core.
            data.append(np.ascontiguousarray(cut.transpose(at.perm)))

        return cls(data, image_width=x, image_length=y, tile_size=tile, bands=b,
                   dtype=arr.dtype, pattern=p, time_count=times)

    def _geometry(self):
        """Return grid dimensions and frame count."""
        return frame_count(self.frame_unit, self.image_width,
                           self.image_length, self.tile_size, self.bands,
                           self.time_count)

    def _at(self, index):
        """Return the location and shape of one frame."""
        return frame_at(self.frame_unit, self.image_width, self.image_length,
                        self.tile_size, self.bands, self.time_count, index)

    @property
    def dims(self):
        """Return the columns that identify each frame."""
        return self.pattern.index_columns(self.bands, self.time_count)

    @property
    def tiles_across(self):
        return self._geometry()[0]

    @property
    def tiles_down(self):
        return self._geometry()[1]

    @property
    def layout(self):
        """Return the decoded frame axis order."""
        return layout_name(self.frame_unit, self.bands, self.time_count)

    @property
    def _per_cell(self):
        """Return the number of frames stored for each grid cell."""
        return len(self._data) // (self.tiles_across * self.tiles_down)

    def _label_head(self, i):
        """Return the band/time portion of a frame label."""
        if not self._walked:
            return None
        return ".".join(str(int(v[i])) for _n, v in self._walked)

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

        Per-position values are repeated for every frame in that cell. Attached
        columns are not written to the rumi file.
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
            # Repeat each cell value for every frame at that grid position.
            col = [values[i // self._per_cell] for i in range(n)]
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
            if self._walked and key == "tile":
                return [self._label(i) for i in range(len(self._data))]
            col = {"row": self._row, "col": self._col}
            for name, values in self._walked:
                col[name] = values
            if key not in col:
                known = (*self.columns, *col, *self._extra)
                raise KeyError(f"no column {key!r}, columns are {known}")
            return col[key].copy()
        # Integer indexing returns a live view of one row.
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
        # Validate all payloads before replacing the column.
        bufs = [None if v is None else _frame_bytes(v, i)
                for i, v in enumerate(values)]
        self._comp = bufs
        self._size = np.array([-1 if b is None else len(b) for b in bufs],
                              np.int64)

    @property
    def _walked(self):
        """Return the band/time columns used to identify frames."""
        have = {"band": self._band, "time": self._time}
        return [(n, have[n]) for n in self.dims[2:]]

    def _label(self, i):
        """Return indexed coordinates followed by the grid position."""
        walked = ".".join(str(int(v[i])) for _n, v in self._walked)
        return f"{walked}.{self._row[i]}.{self._col[i]}"

    def to_pandas(self):
        """Return scalar metadata columns as a DataFrame."""
        import pandas as pd
        cols = {}
        if self._walked:
            cols["tile"] = self["tile"]
        cols["cell"] = self["cell"]
        for name, values in self._walked:
            cols[name] = values
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
                "layout": self.layout, "per": self._per_cell,
                "steps": self.time_count,
                "tiled": bool(self._walked),
                "done": self.done, "dtype": str(self.dtype), "nbytes": nbytes,
                "ratio": raw / nbytes if nbytes else 0.0}

    def _rows(self):
        """Return rows used by the compact representation."""
        n = len(self._data)
        keep = (range(n) if n <= _HEAD_TAIL + 1 else
                [*range(_HEAD), None, *range(n - (_HEAD_TAIL - _HEAD), n)])
        return [None if i is None else
                (i, self._label_head(i),
                 int(self._row[i]), int(self._col[i]),
                 self._data[i].shape, int(self._size[i])) for i in keep]

    @property
    def columns(self):
        """Return the columns shown by the representation."""
        # Frames split by band or time have both frame and cell labels.
        named = ("tile", "cell") if self._walked else ("cell",)
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
        per = self._per_cell
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
    # Validate at assignment so the error identifies the producing frame.
    if not buf:
        raise ValueError(f"frame {i} is empty")
    return buf


def frames(arr, pattern, tile_size=512):
    """Split an array into a FrameTable.

    The left side of the pattern names the input axes. A pair such as ``(row h)``
    splits an image axis into a grid coordinate and a frame-local axis. The
    parenthesized group on the right lists the axes stored together in each frame.

        "b (row h) (col w) -> row col (b h w)"    every band, band planar
        "b (row h) (col w) -> row col (h w b)"    every band, interleaved
        "b (row h) (col w) -> row col b (h w)"    one band per frame

    Only ``b`` and ``t`` are reserved; spatial split names are user-defined.
    """
    return FrameTable.from_array(arr, pattern, tile_size)
