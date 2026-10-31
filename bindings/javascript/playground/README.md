# Rumi playground

Install the JavaScript dependencies and start the local server:

```sh
npm install
npm run playground
```

The server uses the GeoZL WebAssembly package installed from npm. For local
GeoZL development, `GEOZL_WASM_DIR` can point to a directory containing
`geozl_api.js`, `geozl.js` and `geozl.wasm`.

Open `http://127.0.0.1:4321/`. Paste a public `.rumi` URL or drop a local file.
The viewer reads band descriptions, time coordinates, frame layout, tile grid and
georeferencing from the file. Remote servers must allow CORS byte-range requests
and expose `Content-Range`. If they expose a strong `ETag`, they must also allow
the `If-Match` request header.

For a local sample:

```sh
node playground/server.mjs --sample /path/to/image.rumi
```

Then open `http://127.0.0.1:4321/?url=/sample.rumi`.
