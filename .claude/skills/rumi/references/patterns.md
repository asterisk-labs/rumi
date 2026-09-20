# Frame patterns, layouts and output patterns

Sources: `core/src/pattern.cpp` (both grammars), `UNIT_REGISTRY` and `effective_unit` in
`core/include/rumi/rumi.hpp`, `default_pattern` in `core/src/capi.cpp`, the `frame_unit`
and Frame index sections of `SPEC.md`. Shapes and messages below were captured from
0.23.0.

## Contents

1. Frame pattern grammar
2. The `frame_unit` registry
3. Axes of length one
4. Frame order, labels and edge frames
5. Output patterns
6. Choosing a layout
7. Pattern errors

## 1. Frame pattern grammar

A frame pattern tells `rumi.frames` how to cut an array. It is compiled by the core, so
C, Python and future bindings share it.

```text
<input axes> -> <grid y> <grid x> [<index axes>] (<frame axes>)
```

- **Input.** `b` and `t` name the band and time axes. Each parenthesised pair splits one
  image axis into a grid axis and a tile-local axis: the first pair is Y, the second X.
  Split names are free identifiers (`(row h)`, `(y h)`, `(r i)`). The input lists the
  array's axes in the array's own order, so `(row h) (col w) b` cuts a `(Y, X, B)` array.
- **Output.** The two grid names come first, in Y then X order. Band and time axes that
  the frame index walks follow, outermost first. The trailing parenthesised group is the
  decoded frame.
- The frame always holds both tile axes, adjacent and in Y then X order.
- Every input axis is placed exactly once.

```python
rumi.frames(image, "b (row h) (col w) -> row col (b h w)", tile_size=512)   # (B, Y, X)
rumi.frames(hwc, "(row h) (col w) b -> row col (b h w)", tile_size=512)     # (Y, X, B)
rumi.frames(plane, "(row h) (col w) -> row col (h w)", tile_size=512)       # (Y, X)
rumi.frames(cube, "t b (row h) (col w) -> row col (t b h w)", tile_size=512)
```

## 2. The `frame_unit` registry

The file stores the layout as `frame_unit` (IFD tag 65000 and the header). The registry
is append-only. Frame counts are for a `(3, 4, 70, 50)` Cube cut with `tile_size=32`
(a 2 x 3 grid).

| Unit | Decoded frame | Valid when | Pattern output side | Frames |
| ---: | --- | --- | --- | ---: |
| 0 | `h w` | always | `row col b t (h w)` (Image: `row col b (h w)`) | 72 |
| 1 | `b h w` or `t h w` | exactly one of B, T above 1 | `row col (b h w)` | 6 |
| 2 | `h w b` or `h w t` | exactly one of B, T above 1 | `row col (h w b)` | 6 |
| 3 | `b t h w` | B > 1 and T > 1 | `row col (b t h w)` | 6 |
| 4 | `t b h w` | B > 1 and T > 1 | `row col (t b h w)` | 6 |
| 5 | `b h w t` | B > 1 and T > 1 | `row col (b h w t)` | 6 |
| 6 | `t h w b` | B > 1 and T > 1 | `row col (t h w b)` | 6 |
| 7 | `h w b t` | B > 1 and T > 1 | `row col (h w b t)` | 6 |
| 8 | `h w t b` | B > 1 and T > 1 | `row col (h w t b)` | 6 |
| 9 | `h w`, index walks `t` before `b` | B > 1 and T > 1 | `row col t b (h w)` | 72 |

An axis before `h w` is stored as contiguous planes; an axis after `h w` is interleaved
per pixel. Units 0 and 9 decode the same frames and differ only in file order.

A frame holds the tile alone or the tile with every axis longer than one:
`row col t (b h w)` on a Cube raises `PatternError: a frame of (b h w) does not fit 4
bands over 3 time steps; ...`.

## 3. Axes of length one

Band and time axes of extent one are removed before the unit is chosen, and the file
records the resulting unit.

| Input | Pattern output side | Layout | Unit | `dims` |
| --- | --- | --- | ---: | --- |
| `(1, Y, X)` Image | `row col b (h w)` | `h w` | 0 | `row col` |
| `(1, Y, X)` Image | `row col (h w b)` | `h w` | 0 | `row col` |
| `(3, 1, Y, X)` Cube | `row col (t b h w)` | `t h w` | 1 | `row col` |
| `(3, 1, Y, X)` Cube | `row col b t (h w)` | `h w` | 0 | `row col time` |

An Image therefore always uses unit 0, 1 or 2. `FrameTable.layout` and
`Metadata.frame_layout` report the effective axes.

## 4. Frame order, labels and edge frames

```text
tiles_across = ceil(X / tile)      tiles_down = ceil(Y / tile)
spatial      = row * tiles_across + col

cell frames (units 1 to 8):  index = spatial
unit 0:                      index = (spatial * B + b) * T + t
unit 9:                      index = (spatial * T + t) * B + b
```

- `FrameTable` iteration, the file and the header all follow this index.
- `Frame.cell` is `"row.col"`. Tile frames also have `Frame.tile`: the walked
  coordinates in index order, then row and column, so `"band.time.row.col"` for unit 0
  and `"time.band.row.col"` for unit 9 (`"band.row.col"` for an Image).
- Edge frames are clipped, not padded: `h = min(tile, Y - row * tile)` and likewise for
  `w`. A `(4, 1000, 1000)` Image with `tile_size=256` has 16 frames, and the last is
  `(4, 232, 232)`.
- The nominal tile may exceed the image; the single frame is then the whole image.

## 5. Output patterns

`read` and `read_many` take a second, simpler grammar that only arranges the result.

- Letters `n t b y x`, each at most once, spaces optional; `y` and `x` are required.
- Order is free: `"y x b"` returns channels last and `"b x y"` transposes the image axes.
- Parentheses merge adjacent axes into one dimension, the left axis varying slowest:
  `"(t b) y x"` equals `result.reshape(T * B, Y, X)`. Groups do not nest.
- An omitted axis must have extent one after selection (`b > 1 needs b in the pattern`).
  An axis of extent one may still be placed.
- Defaults come from the file's time count, not the selection: `b y x` for an Image,
  `t b y x` for a Cube, and `n b y x` or `n t b y x` for `read_many`.

| Call | Shape |
| --- | --- |
| `read(image4, h)` | `(4, 1000, 1000)` |
| `read(image4, h, pattern="y x b")` | `(1000, 1000, 4)` |
| `read(image4, h, bands=[2], pattern="y x")` | `(1000, 1000)` |
| `read(image4, h, pattern="n b y x")` | `(1, 4, 1000, 1000)` |
| `read(cube, h, time=[1])` | `(1, 4, 300, 260)` |
| `read(cube, h, time=[1], pattern="b y x")` | `(4, 300, 260)` |
| `read(cube, h, pattern="(t b) y x")` | `(12, 300, 260)` |
| `read_many([a, b], [ha, hb], windows=w, bands=[2, 0], pattern="n y x b")` | `(2, 64, 64, 2)` |

The output pattern never changes what is decoded; only the frame layout does.

## 6. Choosing a layout

What a read decodes for a window touching `k` tile positions, `S` selected bands and `U`
selected steps:

| Layout | Frames decoded | Samples per frame |
| --- | --- | --- |
| `h w` (units 0, 9) | `k * S * U` | `h * w` |
| cell layouts (units 1 to 8) | `k` | `B * T * h * w`, whatever the selection |

- **Tile frames** make band and time selections cheap and let every band use its own
  graph, but OpenZL never sees two bands together.
- **Cell frames** let one graph model a whole cell. Reading one band of a 13-band cell
  decodes all 13.
- **Units 0 and 9** change which frames sit next to each other. Unit 0 keeps every step
  of one band adjacent; unit 9 keeps every band of one step adjacent. Adjacent ranges can
  be merged into fewer remote requests.
- **Tile size.** A window aligned to the grid touches the fewest frames. Every frame adds
  to the header; when frame sizes vary, a file holds at most 5,592,405 frames (the
  writer refuses more).
- **Planar or interleaved** cell layouts compress differently with the same recipe.
  Measure with GeoZL on representative frames (`writing.md`).

## 7. Pattern errors

Frame patterns raise `PatternError` (a `ValueError`) from `rumi.frames`; the array rank
check raises a plain `ValueError`.

| Pattern or call | Message |
| --- | --- |
| `b (row h) (col w) row col (b h w)` | `a pattern reads 'input -> output' and needs exactly one '->'` |
| `... -> col row (b h w)` | `the grid axes lead, in Y then X order: expected 'row col' first, got 'col row'` |
| `... -> row col (b h w) b` | `the frame is the trailing parenthesised group, so the pattern must end in one; got 'b' last` |
| `... -> row col (h b w)` or `(w h b)` | `the tile axes stay adjacent and in Y then X order, so 'h' must be followed by 'w'` |
| `... -> row col h (b w)` | `'h' belongs to the tile, so it goes in the frame, not between the grid and it` |
| `... -> row col (h w)` with `b` on the left | `'b' named on the left but never placed` |
| `c (row h) (col w) -> row col (c h w)` | `'c' is not split, so it must name an axis a frame may hold; a spatial axis is written like '(c h)'` |
| `b (row h) -> row (b h)` | `the input needs exactly two split axes, the spatial ones` |
| `... -> row col (b h w` | `unbalanced '(' in the output` |
| a 2-axis pattern on a 3-D array | `the pattern names 2 axes (y x), got shape (4, 70, 50)` |
| `tile_size=0` | `tile_size must be in [1, 65535], got 0` |

Output patterns raise `ValueError` from `read`:

| Pattern | Message |
| --- | --- |
| `B Y X` | `unknown axis 'B' (expected n, t, b, y, x)` |
| `b y y x` | `axis 'y' used more than once` |
| `(b (y) x)` | `nested parentheses are not allowed` |
| `b y` | `pattern must contain y and x` |
| `y x` with 4 bands | `b > 1 needs b in the pattern` |
| `b y x` on a whole Cube | `t > 1 needs t in the pattern` |
| `b y x` in `read_many` with two items | `n > 1 needs n in the pattern` |
