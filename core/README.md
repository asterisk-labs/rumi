# rumi core

C++ GDAL driver and read paths for the rumi profile, a tiled little-endian BigTIFF whose tiles are self-contained OpenZL frames (compression 60000).

Bindings live in `../bindings/`. The driver registers under the name `RUMI` and activates only when the `RUMI_HEADER` open option is set, so it never claims plain TIFFs.