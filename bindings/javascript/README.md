# Rumi for JavaScript

This package reads Rumi Image and Cube files in browsers and Node.js. It parses
the file directory and trailer, requests only the needed byte ranges, and decodes
each frame with the GeoZL WebAssembly package.

GeoZL uses wasm64, so decoding requires WebAssembly Memory64. Use Node.js 24 or
newer, or a browser with Memory64 support.

```js
import { openRumi } from "@asterisk-labs/rumi";

const dataset = await openRumi("https://example.com/cube.rumi");

const chip = await dataset.read({
  time: [3],
  bands: [4, 2, 1],
  window: [1024, 2048, 256, 256],
});

console.log(chip.shape); // [1, 3, 256, 256]
console.log(chip.data);  // dtype-sized typed storage
console.log(dataset.info().bands); // descriptions stored in the Rumi trailer
console.log(dataset.info().time);  // exact POSIX seconds as bigint values
```

Indices are zero-based. A window is `[y, x, height, width]`. Images return
`[band, y, x]`; cubes return `[time, band, y, x]`. Reordered and repeated band
or time selections are preserved.

`data` is a native typed view when JavaScript has the exact scalar type.
Float16, bfloat16, float8 and complex values use their component storage view;
`dtype` describes how to interpret it. `bytes` always exposes the same result as
raw little-endian bytes.

## Sources

`openRumi` accepts an `ArrayBuffer`, typed array, `Blob`, URL, or a custom source:

```js
const source = {
  async read(offset, length) {
    // Return exactly `length` bytes as Uint8Array.
  },
  async size() {
    // Return the complete .rumi file size as bigint.
  },
};
```

URL reads require HTTP range support. Cross-origin responses must expose
`Content-Range`. If they also expose a strong `ETag`, CORS must allow the
`If-Match` request header; Rumi uses it to pin subsequent reads to that object
version. A wrong range or changed object is rejected.

Use `planRanges()` to inspect the compressed frames needed by a selection
without fetching or decoding them.

Opening rejects more than one million time coordinates by default. Set
`maxTimeCoordinates` explicitly when a larger axis is expected.

## Playground

`npm run playground` opens a small MapLibre viewer. Paste a public `.rumi` URL
or drop a local file, choose bands and render it over the map. See
[`playground/README.md`](playground/README.md) for setup.

## Scope

The reader accepts complete `.rumi` files. It exposes the array shape, dtype,
tile grid, frame index, affine transform, EPSG CRS, band descriptions and time
coordinates. Time values are exact POSIX seconds represented as `bigint`;
intervals are `[start, end]` pairs. Writing files and opening external `LOVE`
headers are outside this reader. Pass `decoder` only to override GeoZL.
