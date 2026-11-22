# Rumi.jl

Julia reader and writer for Rumi Image and Cube files. It uses the same
external-header API as the Python and R bindings.

```julia
using Rumi

header = Base.read("scene.header")
chip = Rumi.read("scene.rumi", header; bands=[4, 8], time=1:3,
                 window=(1, 1, 256, 256))
meta = Rumi.info("scene.rumi"; header)
```

A source is a local path, a remote URI or the file's bytes. Every call takes
the source and its header and keeps nothing between calls. A read uses the
header alone, and `Rumi.info(source; header)` checks that the two match.
`Rumi.read_many(sources, headers; windows)` reads one window from each source
into one batch. `Rumi.info(source)` reads a file without its external header
and rebuilds it in `meta.header`, and `Rumi.info(; header)` inspects a header
alone.

Selections and window origins are one-based in Julia. A selection is an
integer, a vector or a range such as `2:5`, and a tuple is refused. The window
order is `(row, column, height, width)`. Arrays use Julia's native memory
order, so an Image is shaped `(X, Y, Band)` and a Cube `(X, Y, Band, Time)`.
Metadata keeps Rumi's canonical `(Band, Y, X)` or `(Time, Band, Y, X)` shape.

Invalid requests throw `ArgumentError`. Other failures throw `RumiError`, whose
`kind` is a symbol such as `:io`, `:decode` or `:state`. Every function takes
the `Rumi.` prefix, and the package exports only its types.

Set `RUMI_LIB` to a built `librumi` while developing. A package release downloads
the matching native artifact on first use and keeps it in the Julia depot.

## Writing

`Rumi.frames` cuts an array into frames, you compress each one with the
[GeoZL](https://asterisk.coop/geozl/) built into the package, `Rumi.GeoZL`, and
`Rumi.write` stores the payloads and returns the external header. `Rumi.GeoZL`
covers `graph` and `compress` only, with argument errors that read as in the
GeoZL packages; `decompress`, `profile` and `coeffs` live in
[GeoZL.jl](https://asterisk.coop/geozl/).

```julia
using Dates
using Rumi
using Rumi: GeoZL

image = rand(UInt16(0):UInt16(4095), 1024, 1024, 4)
table = Rumi.frames(image, "b (row h) (col w) -> row col b (h w)"; tile_size=256)
for (i, tile) in enumerate(table.data)
    table.compressed[i] = GeoZL.compress(tile, GeoZL.graph(tile, "planar>zigzag>pfor"))
end
header = Rumi.write("scene.rumi", table; bands=["B02", "B03", "B04", "B08"],
                    time=[Date(2025, 8, 8)],
                    transform=(10, 0, 500000, 0, -10, 8000000), crs=32618)
Base.write("scene.header", header)
```

Patterns use the same axis names as Python. Julia holds those axes in reverse,
so this image is `(X, Y, Band)`, and each frame is ready for `Rumi.GeoZL.graph`.
The array's element type sets the file's sample type. Each table records where
its frames go in `row`, `col`, `band` and `time`, and
`Rumi.frame_start(size(image), pattern, 256)` gives the first frame's offset
before anything is compressed.
