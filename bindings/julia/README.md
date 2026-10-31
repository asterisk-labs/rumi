# Rumi.jl

Julia reader for Rumi Image and Cube files. It uses the same external-header
API as the Python and R bindings.

```julia
using Rumi

file = Rumi.open("scene.rumi", Base.read("scene.header"))
chip = Rumi.read(file; bands=[4, 8], time=1:3,
                 window=(1, 1, 256, 256))
meta = Rumi.info(file)
close(file)
```

Selections and window origins are one-based in Julia. The window order is
`(row, column, height, width)`. Arrays use Julia's native memory order, so an
Image is shaped `(X, Y, Band)` and a Cube `(X, Y, Band, Time)`. Metadata keeps
Rumi's canonical `(Band, Y, X)` or `(Time, Band, Y, X)` shape.

Set `RUMI_LIB` to a built `librumi` while developing. A package release downloads
the matching native artifact on first use and keeps it in the Julia depot.
