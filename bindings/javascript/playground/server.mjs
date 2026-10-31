import { createReadStream } from "node:fs";
import { access, stat } from "node:fs/promises";
import http from "node:http";
import path from "node:path";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
const javascript = path.resolve(here, "..");
let installedGeoZL;
try {
  installedGeoZL = path.dirname(fileURLToPath(import.meta.resolve("@asterisk-labs/geozl")));
} catch {}
const candidates = [
  process.env.GEOZL_WASM_DIR,
  installedGeoZL,
  path.resolve(here, "../../../../geozl/core/build-wasm/wasm"),
  path.resolve(here, "../../../extern/geozl/core/build-wasm/wasm"),
].filter(Boolean);
let geozl;
for (const candidate of candidates) {
  try {
    await access(path.join(candidate, "geozl_api.js"));
    geozl = candidate;
    break;
  } catch {}
}
if (!geozl) throw new Error("GeoZL WASM build not found; set GEOZL_WASM_DIR");

const sampleFlag = process.argv.indexOf("--sample");
const sample = sampleFlag >= 0 ? path.resolve(process.argv[sampleFlag + 1]) : undefined;
const port = Number(process.env.PORT || 4321);

function inside(root, requestPath) {
  const file = path.resolve(root, requestPath);
  return file === root || file.startsWith(`${root}${path.sep}`) ? file : undefined;
}

function target(url) {
  if (url === "/sample.rumi" && sample) return sample;
  if (url.startsWith("/rumi/")) return inside(javascript, url.slice(6));
  if (url.startsWith("/geozl/")) return inside(geozl, url.slice(7));
  return inside(here, url === "/" ? "index.html" : url.slice(1));
}

http.createServer(async (request, response) => {
  try {
    const file = target(new URL(request.url, "http://localhost").pathname);
    if (!file) return void response.writeHead(404).end("not found");
    const info = await stat(file);
    const match = /^bytes=(\d+)-(\d+)$/.exec(request.headers.range ?? "");
    const extension = path.extname(file);
    const type = extension === ".wasm" ? "application/wasm"
      : extension === ".js" || extension === ".mjs" ? "text/javascript"
      : extension === ".css" ? "text/css"
      : extension === ".svg" ? "image/svg+xml"
      : extension === ".html" ? "text/html; charset=utf-8"
      : "application/octet-stream";
    const headers = { "Accept-Ranges": "bytes", "Access-Control-Allow-Origin": "*", "Content-Type": type };
    if (match) {
      const start = Number(match[1]);
      const end = Number(match[2]);
      if (start > end || end >= info.size) {
        return void response.writeHead(416, { "Content-Range": `bytes */${info.size}` }).end();
      }
      response.writeHead(206, {
        ...headers,
        "Content-Length": end - start + 1,
        "Content-Range": `bytes ${start}-${end}/${info.size}`,
      });
      createReadStream(file, { start, end }).pipe(response);
      return;
    }
    response.writeHead(200, { ...headers, "Content-Length": info.size });
    createReadStream(file).pipe(response);
  } catch (error) {
    response.writeHead(404).end(error.message ?? String(error));
  }
}).listen(port, "127.0.0.1", () => {
  console.log(`Rumi playground: http://127.0.0.1:${port}/`);
});
