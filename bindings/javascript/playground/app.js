import { openRumi } from "/rumi/index.js";
import * as maplibregl from "https://cdn.jsdelivr.net/npm/maplibre-gl@6.9.0/dist/maplibre-gl.mjs";
import { affineCorners, rasterRgba } from "./render.js";

const SOURCE = "rumi-image";
const LAYER = "rumi-image";
const PROJ4_URL = "https://cdn.jsdelivr.net/npm/proj4@2.15.0/+esm";
const DATE_LIMIT_MS = 8_640_000_000_000_000n;
const MAX_PREVIEW_PIXELS = 2048 * 2048;

const element = Object.fromEntries([
  "url-form", "rumi-url", "file-input", "status", "message", "controls",
  "source-name", "source-info", "time-field", "time", "band-fields", "red",
  "green", "blue", "opacity", "opacity-value", "render", "close", "busy",
  "drop-zone", "metadata-panel", "metadata-title", "metadata-body",
  "show-metadata",
].map((id) => [id.replaceAll("-", ""), document.getElementById(id)]));

const map = new maplibregl.Map({
  container: "map",
  center: [-76.9, -12.05],
  zoom: 8,
  attributionControl: false,
  style: {
    version: 8,
    sources: {
      basemap: {
        type: "raster",
        tiles: ["https://server.arcgisonline.com/ArcGIS/rest/services/Canvas/World_Light_Gray_Base/MapServer/tile/{z}/{y}/{x}"],
        tileSize: 256,
        maxzoom: 16,
      },
      labels: {
        type: "raster",
        tiles: ["https://server.arcgisonline.com/ArcGIS/rest/services/Canvas/World_Light_Gray_Reference/MapServer/tile/{z}/{y}/{x}"],
        tileSize: 256,
        maxzoom: 16,
      },
    },
    layers: [
      { id: "basemap", type: "raster", source: "basemap" },
      { id: "labels", type: "raster", source: "labels" },
    ],
  },
});
map.addControl(new maplibregl.NavigationControl(), "bottom-right");
const mapReady = new Promise((resolve) => map.on("load", resolve));
let dataset;

function setStatus(text, state = "ready") {
  element.status.className = `status ${state}`;
  element.status.querySelector("span").textContent = text;
}

function showError(text) {
  element.message.textContent = text;
  element.message.className = "message error";
  element.message.hidden = false;
  setStatus("Error", "error");
}

function clearMessage() {
  element.message.hidden = true;
  element.message.textContent = "";
}

function busy(active) {
  element.busy.hidden = !active;
}

function formatInstant(seconds) {
  const milliseconds = seconds * 1000n;
  if (milliseconds >= -DATE_LIMIT_MS && milliseconds <= DATE_LIMIT_MS) {
    return new Date(Number(milliseconds)).toISOString().replace(".000Z", "Z");
  }
  return `${seconds} seconds since 1970-01-01T00:00:00Z`;
}

function formatTime(value) {
  return Array.isArray(value)
    ? `${formatInstant(value[0])} — ${formatInstant(value[1])}`
    : formatInstant(value);
}

function optionText(label, index) {
  return label ? `${index} · ${label}` : `Band ${index}`;
}

function bandOptions(select, labels, count, selected) {
  select.replaceChildren(...Array.from({ length: count }, (_, band) => {
    const option = document.createElement("option");
    option.value = band;
    option.textContent = optionText(labels?.[band], band);
    option.title = labels?.[band] ?? `Band ${band}`;
    option.selected = band === selected;
    return option;
  }));
}

function namedBand(labels, expression, fallback, count) {
  const found = labels?.findIndex((label) => expression.test(label)) ?? -1;
  return found >= 0 ? found : Math.min(fallback, count - 1);
}

function metadataRow(name, value, code = false) {
  const row = document.createElement("div");
  row.className = "metadata-row";
  const term = document.createElement("dt");
  term.textContent = name;
  const description = document.createElement("dd");
  if (code) {
    const block = document.createElement("span");
    block.className = "code-value";
    block.textContent = value;
    description.append(block);
  } else {
    description.textContent = value;
  }
  row.append(term, description);
  return row;
}

function metadataGroup(title, rows) {
  const section = document.createElement("section");
  section.className = "metadata-group";
  const heading = document.createElement("h3");
  heading.textContent = title;
  const list = document.createElement("dl");
  for (const row of rows) list.append(metadataRow(...row));
  section.append(heading, list);
  return section;
}

function bandGroup(labels, count) {
  const section = document.createElement("section");
  section.className = "metadata-group";
  const heading = document.createElement("h3");
  heading.textContent = `Bands · ${count}`;
  const list = document.createElement("ol");
  list.className = "band-list";
  for (let band = 0; band < count; band += 1) {
    const item = document.createElement("li");
    const index = document.createElement("b");
    index.textContent = String(band).padStart(2, "0");
    const text = document.createElement("span");
    text.textContent = labels?.[band] ?? `Band ${band}`;
    item.append(index, text);
    list.append(item);
  }
  section.append(heading, list);
  return section;
}

function showMetadata(info, name) {
  const bandCount = info.shape.length === 3 ? info.shape[0] : info.shape[1];
  const timeCount = info.shape.length === 4 ? info.shape[0] : 1;
  element.metadatatitle.textContent = name;
  element.metadatabody.replaceChildren(
    metadataGroup("Array", [
      ["Shape", info.shape.join(" × ")],
      ["Axes", info.axes.join(" · ")],
      ["Data type", `${info.dtype.name} · ${info.dtype.bits} bit`],
      ["Tile", info.tileShape.join(" × ")],
      ["Tile grid", info.gridShape.join(" × ")],
      ["Frame layout", info.frameLayout],
      ["Frames", String(info.frameCount)],
    ]),
    metadataGroup("Acquisition", [
      ["Kind", info.timeKind ?? "Unavailable"],
      ["Steps", String(timeCount)],
      ...(info.time?.map((value, index) => [`Time ${index}`, formatTime(value)]) ?? []),
    ]),
    metadataGroup("Georeferencing", [
      ["CRS", info.crs ?? "Undefined"],
      ["Raster type", info.pixelIsPoint ? "Pixel is point" : "Pixel is area"],
      ["Transform", info.transform?.map((value) => Number(value).toPrecision(10)).join(", ") ?? "Undefined", true],
    ]),
    bandGroup(info.bands, bandCount),
  );
  element.metadatapanel.hidden = false;
  element.metadatapanel.setAttribute("aria-hidden", "false");
  requestAnimationFrame(() => element.metadatapanel.classList.add("open"));
}

function setupControls(info, name) {
  const bandCount = info.shape.length === 3 ? info.shape[0] : info.shape[1];
  const timeCount = info.shape.length === 4 ? info.shape[0] : 1;
  element.sourcename.textContent = name;
  element.sourceinfo.textContent = `${info.shape.join(" × ")} · ${info.dtype.name} · ${info.crs ?? "no CRS"}`;
  element.timefield.hidden = info.time === null;
  element.time.replaceChildren(...Array.from({ length: timeCount }, (_, time) => {
    const option = document.createElement("option");
    option.value = time;
    option.textContent = info.time?.[time] !== undefined ? formatTime(info.time[time]) : `Time ${time}`;
    return option;
  }));
  const red = namedBand(info.bands, /^B0?4\b/i, 0, bandCount);
  const green = namedBand(info.bands, /^B0?3\b/i, 1, bandCount);
  const blue = namedBand(info.bands, /^B0?2\b/i, 2, bandCount);
  bandOptions(element.red, info.bands, bandCount, red);
  bandOptions(element.green, info.bands, bandCount, green);
  bandOptions(element.blue, info.bands, bandCount, blue);
  element.bandfields.hidden = bandCount === 1;
  element.controls.hidden = false;
  showMetadata(info, name);
}

function supportedDtype(dtype) {
  return !dtype.name.startsWith("complex")
    && !dtype.name.startsWith("float8")
    && !["float16", "bfloat16"].includes(dtype.name);
}

async function projector(epsg) {
  if (epsg === 4326) return (point) => point;
  if (epsg === 3857) {
    return ([x, y]) => [
      (x / 6378137) * (180 / Math.PI),
      (2 * Math.atan(Math.exp(y / 6378137)) - Math.PI / 2) * (180 / Math.PI),
    ];
  }
  const north = epsg >= 32601 && epsg <= 32660;
  const south = epsg >= 32701 && epsg <= 32760;
  if (!north && !south) throw new Error(`The viewer cannot project EPSG:${epsg} yet.`);
  const zone = epsg - (south ? 32700 : 32600);
  const proj4 = (await import(PROJ4_URL)).default;
  const definition = `+proj=utm +zone=${zone}${south ? " +south" : ""} +datum=WGS84 +units=m +no_defs`;
  return (point) => proj4(definition, "EPSG:4326", point);
}

async function imageCorners(info) {
  if (info.transform && info.epsg) {
    const toLonLat = await projector(info.epsg);
    return {
      corners: affineCorners(info.transform, info.shape.at(-1), info.shape.at(-2)).map(toLonLat),
      fallback: false,
    };
  }
  const bounds = map.getBounds();
  return {
    corners: [bounds.getNorthWest(), bounds.getNorthEast(), bounds.getSouthEast(), bounds.getSouthWest()]
      .map(({ lng, lat }) => [lng, lat]),
    fallback: true,
  };
}

async function render() {
  if (!dataset) return;
  busy(true);
  clearMessage();
  setStatus("Rendering", "loading");
  try {
    const info = dataset.info();
    if (!supportedDtype(info.dtype)) throw new Error(`${info.dtype.name} rendering is not supported yet.`);
    const bandCount = info.shape.length === 3 ? info.shape[0] : info.shape[1];
    const width = info.shape.at(-1);
    const height = info.shape.at(-2);
    if (width * height > MAX_PREVIEW_PIXELS) {
      throw new Error("This image is too large for the browser preview (maximum 2048 × 2048 pixels).");
    }
    const bands = bandCount === 1
      ? [0]
      : [element.red, element.green, element.blue].map((select) => Number(select.value));
    const result = await dataset.read({
      time: [Number(element.time.value || 0)],
      bands,
    });
    const image = rasterRgba(result.data, width, height, bands.length);
    const canvas = document.createElement("canvas");
    canvas.width = image.width;
    canvas.height = image.height;
    canvas.getContext("2d").putImageData(new ImageData(image.rgba, image.width, image.height), 0, 0);
    const placement = await imageCorners(info);

    await mapReady;
    if (map.getLayer(LAYER)) map.removeLayer(LAYER);
    if (map.getSource(SOURCE)) map.removeSource(SOURCE);
    map.addSource(SOURCE, { type: "image", url: canvas.toDataURL(), coordinates: placement.corners });
    map.addLayer({
      id: LAYER,
      type: "raster",
      source: SOURCE,
      paint: { "raster-opacity": Number(element.opacity.value) / 100, "raster-fade-duration": 0 },
    }, "labels");
    const bounds = new maplibregl.LngLatBounds();
    placement.corners.forEach((corner) => bounds.extend(corner));
    const padding = window.innerWidth > 900
      ? { top: 70, right: 420, bottom: 70, left: 380 }
      : 45;
    map.fitBounds(bounds, { padding, maxZoom: 15, duration: 700 });
    setStatus(placement.fallback ? "No CRS" : "Rendered", "ready");
  } catch (error) {
    showError(error.message ?? String(error));
  } finally {
    busy(false);
  }
}

async function openSource(source, name) {
  busy(true);
  clearMessage();
  setStatus("Reading", "loading");
  try {
    dataset = await openRumi(source);
    setupControls(dataset.info(), name);
    await render();
  } catch (error) {
    dataset = undefined;
    element.controls.hidden = true;
    element.metadatapanel.hidden = true;
    showError(error.message ?? String(error));
  } finally {
    busy(false);
  }
}

function hideMetadata() {
  element.metadatapanel.classList.remove("open");
  element.metadatapanel.setAttribute("aria-hidden", "true");
  setTimeout(() => {
    if (!element.metadatapanel.classList.contains("open")) element.metadatapanel.hidden = true;
  }, 200);
}

element.urlform.addEventListener("submit", (event) => {
  event.preventDefault();
  const url = element.rumiurl.value.trim();
  if (url) void openSource(url, url.split("/").pop() || url);
});

element.fileinput.addEventListener("change", () => {
  const [file] = element.fileinput.files;
  if (file) void openSource(file, file.name);
});

let dragDepth = 0;
document.addEventListener("dragenter", (event) => {
  event.preventDefault();
  dragDepth += 1;
  element.dropzone.hidden = false;
});
document.addEventListener("dragover", (event) => event.preventDefault());
document.addEventListener("dragleave", (event) => {
  event.preventDefault();
  dragDepth = Math.max(0, dragDepth - 1);
  if (dragDepth === 0) element.dropzone.hidden = true;
});
document.addEventListener("drop", (event) => {
  event.preventDefault();
  dragDepth = 0;
  element.dropzone.hidden = true;
  const file = [...event.dataTransfer.files].find((candidate) => candidate.name.endsWith(".rumi"));
  if (file) void openSource(file, file.name);
  else showError("Drop a .rumi file.");
});

element.render.addEventListener("click", () => void render());
element.opacity.addEventListener("input", () => {
  element.opacityvalue.value = `${element.opacity.value}%`;
  if (map.getLayer(LAYER)) map.setPaintProperty(LAYER, "raster-opacity", Number(element.opacity.value) / 100);
});
element.close.addEventListener("click", hideMetadata);
element.showmetadata.addEventListener("click", () => {
  if (!dataset) return;
  element.metadatapanel.hidden = false;
  element.metadatapanel.setAttribute("aria-hidden", "false");
  requestAnimationFrame(() => element.metadatapanel.classList.add("open"));
});

const initialUrl = new URL(window.location.href).searchParams.get("url");
if (initialUrl) {
  element.rumiurl.value = initialUrl;
  void openSource(initialUrl, initialUrl.split("/").pop() || initialUrl);
}
