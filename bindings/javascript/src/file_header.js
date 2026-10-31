import { derivedBaseOffset, makeExternalHeader } from "./header.js";
import { epsgKind } from "./epsg.js";
import { readExact } from "./source.js";
import { readTrailer } from "./trailer.js";

const FILE_HEADER_SIZE = 16;
const IFD_OFFSET = 16;
const IFD_ENTRIES = 13;
const IFD_ENTRY_SIZE = 20;
const IFD_SIZE = 8 + IFD_ENTRIES * IFD_ENTRY_SIZE + 8;
const MAX_U32 = 0xffffffffn;
const MAX_U64 = (1n << 64n) - 1n;

const SHORT = 3;
const LONG = 4;
const DOUBLE = 12;
const LONG8 = 16;

const required = [
  [256, LONG, true],
  [257, LONG, true],
  [258, SHORT, false],
  [277, SHORT, true],
  [322, SHORT, true],
  [323, SHORT, true],
  [324, LONG8, false],
  [325, LONG, false],
  [339, SHORT, false],
  [34264, DOUBLE, false],
  [34735, SHORT, false],
  [65000, SHORT, true],
  [65001, LONG, true],
];

const typeBytes = new Map([[SHORT, 2], [LONG, 4], [DOUBLE, 8], [LONG8, 8]]);

function numberOf(value, label) {
  if (value > BigInt(Number.MAX_SAFE_INTEGER)) throw new RangeError(`${label} is too large`);
  return Number(value);
}

function entryValue(entry, width, index = 0) {
  const offset = index * width;
  if (width === 2) return BigInt(entry.data.getUint16(offset, true));
  if (width === 4) return BigInt(entry.data.getUint32(offset, true));
  return entry.data.getBigUint64(offset, true);
}

function entryBytes(metadata, entry, maxMetadataBytes) {
  const width = typeBytes.get(entry.type);
  const total = entry.count * BigInt(width);
  if (total > BigInt(maxMetadataBytes)) throw new RangeError(`tag ${entry.tag} exceeds maxMetadataBytes`);
  const size = numberOf(total, `tag ${entry.tag}`);
  if (size <= 8) return entry.raw.slice(0, size);
  const offset = new DataView(entry.raw.buffer, entry.raw.byteOffset, 8).getBigUint64(0, true);
  const start = numberOf(offset, `tag ${entry.tag} offset`);
  if (start > metadata.byteLength || size > metadata.byteLength - start) {
    throw new RangeError(`tag ${entry.tag} leaves the metadata region`);
  }
  return metadata.subarray(start, start + size);
}

function integerValues(metadata, entry, expected, maxMetadataBytes) {
  if (entry.count !== BigInt(expected)) {
    throw new Error(`tag ${entry.tag} has ${entry.count} values, expected ${expected}`);
  }
  const width = typeBytes.get(entry.type);
  const bytes = entryBytes(metadata, entry, maxMetadataBytes);
  const data = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const readable = { ...entry, data };
  const out = new Array(expected);
  for (let index = 0; index < expected; index += 1) {
    out[index] = entryValue(readable, width, index);
  }
  return out;
}

function doubleValues(metadata, entry, expected, maxMetadataBytes) {
  if (entry.count !== BigInt(expected) || entry.type !== DOUBLE) {
    throw new Error(`tag ${entry.tag} must contain ${expected} doubles`);
  }
  const bytes = entryBytes(metadata, entry, maxMetadataBytes);
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  return Array.from({ length: expected }, (_, index) => view.getFloat64(index * 8, true));
}

function scalar(metadata, entries, tag, maxMetadataBytes) {
  const entry = entries.get(tag);
  return integerValues(metadata, entry, 1, maxMetadataBytes)[0];
}

export async function externalHeaderFromFile(source, {
  maxMetadataBytes,
  maxTimeCoordinates,
}) {
  const fixed = await readExact(source, 0n, IFD_OFFSET + IFD_SIZE);
  const view = new DataView(fixed.buffer, fixed.byteOffset, fixed.byteLength);
  if (fixed[0] !== 0x52 || fixed[1] !== 0x55 || fixed[2] !== 0x4d || fixed[3] !== 0x49) {
    throw new Error("source is not a Rumi file");
  }
  if (view.getUint16(4, true) !== 1) throw new Error("unsupported Rumi file version");
  if (view.getUint16(6, true) !== 0) throw new Error("Rumi file reserved field is not zero");
  if (view.getBigUint64(8, true) !== BigInt(IFD_OFFSET)) throw new Error("Rumi IFD is not at byte 16");
  if (view.getBigUint64(IFD_OFFSET, true) !== BigInt(IFD_ENTRIES)) {
    throw new Error("Rumi file must contain exactly 13 IFD entries");
  }
  if (view.getBigUint64(IFD_OFFSET + 8 + IFD_ENTRIES * IFD_ENTRY_SIZE, true) !== 0n) {
    throw new Error("Rumi file contains more than one IFD");
  }

  const entries = new Map();
  let externalCursor = BigInt(IFD_OFFSET + IFD_SIZE);
  for (let index = 0; index < required.length; index += 1) {
    const at = IFD_OFFSET + 8 + index * IFD_ENTRY_SIZE;
    const [expectedTag, expectedType, single] = required[index];
    const tag = view.getUint16(at, true);
    const type = view.getUint16(at + 2, true);
    const count = view.getBigUint64(at + 4, true);
    if (tag !== expectedTag || type !== expectedType) {
      throw new Error(`IFD entry ${index} does not match the Rumi profile`);
    }
    if (single && count !== 1n) throw new Error(`tag ${tag} must contain one value`);
    const width = typeBytes.get(type);
    if (count > MAX_U64 / BigInt(width)) throw new RangeError(`tag ${tag} size overflows uint64`);
    const total = count * BigInt(width);
    const raw = fixed.slice(at + 12, at + 20);
    if (total <= 8n) {
      for (let byte = Number(total); byte < 8; byte += 1) {
        if (raw[byte] !== 0) throw new Error(`tag ${tag} has non-zero inline padding`);
      }
    } else {
      const offset = new DataView(raw.buffer, raw.byteOffset, 8).getBigUint64(0, true);
      if (offset !== externalCursor) throw new Error(`tag ${tag} external value is not contiguous`);
      externalCursor += total + (total & 1n);
      if (externalCursor > BigInt(maxMetadataBytes)) {
        throw new RangeError("Rumi file metadata exceeds maxMetadataBytes");
      }
    }
    entries.set(tag, { tag, type, count, raw });
  }

  const externalSize = numberOf(externalCursor - BigInt(fixed.byteLength), "Rumi metadata");
  const metadata = new Uint8Array(fixed.byteLength + externalSize);
  metadata.set(fixed);
  if (externalSize !== 0) {
    metadata.set(await readExact(source, BigInt(fixed.byteLength), externalSize), fixed.byteLength);
  }

  if (entries.get(34264).count !== 16n || entries.get(34735).count !== 16n) {
    throw new Error("Rumi transform and GeoKey arrays must contain 16 values");
  }

  const fields = {
    width: numberOf(scalar(metadata, entries, 256, maxMetadataBytes), "width"),
    height: numberOf(scalar(metadata, entries, 257, maxMetadataBytes), "height"),
    bands: numberOf(scalar(metadata, entries, 277, maxMetadataBytes), "bands"),
    tileWidth: numberOf(scalar(metadata, entries, 322, maxMetadataBytes), "tileWidth"),
    tileHeight: numberOf(scalar(metadata, entries, 323, maxMetadataBytes), "tileHeight"),
    frameUnit: numberOf(scalar(metadata, entries, 65000, maxMetadataBytes), "frameUnit"),
    timeCount: numberOf(scalar(metadata, entries, 65001, maxMetadataBytes), "timeCount"),
  };
  if (fields.width === 0 || fields.height === 0 || fields.bands === 0
      || fields.tileWidth === 0 || fields.tileHeight === 0 || fields.timeCount === 0) {
    throw new RangeError("invalid Rumi dimensions");
  }

  const bits = integerValues(metadata, entries.get(258), fields.bands, maxMetadataBytes);
  const formats = integerValues(metadata, entries.get(339), fields.bands, maxMetadataBytes);
  if (bits.some((value) => value !== bits[0])) throw new Error("mixed BitsPerSample is not supported");
  if (formats.some((value) => value !== formats[0])) throw new Error("mixed SampleFormat is not supported");
  if (bits[0] > 255n || formats[0] > 255n) {
    throw new Error(`unsupported sample format ${formats[0]}/${bits[0]}`);
  }
  fields.bits = numberOf(bits[0], "BitsPerSample");
  fields.sampleFormat = numberOf(formats[0], "SampleFormat");

  const matrix = doubleValues(metadata, entries.get(34264), 16, maxMetadataBytes);
  if (matrix.some((value) => !Number.isFinite(value))) {
    throw new Error("Rumi transformation contains a non-finite value");
  }
  for (const index of [2, 6, 8, 9, 10, 11, 12, 13, 14]) {
    if (matrix[index] !== 0) throw new Error(`Rumi transformation element ${index} must be zero`);
  }
  if (matrix[15] !== 1) throw new Error("Rumi transformation element 15 must be one");

  const geokeys = integerValues(metadata, entries.get(34735), 16, maxMetadataBytes);
  const expectedHeader = [1n, 1n, 0n, 3n];
  if (expectedHeader.some((value, index) => geokeys[index] !== value)) {
    throw new Error("invalid Rumi GeoKey directory header");
  }
  for (let key = 0; key < 3; key += 1) {
    const at = 4 + key * 4;
    if (geokeys[at + 1] !== 0n || geokeys[at + 2] !== 1n) {
      throw new Error("Rumi GeoKeys must store one inline value");
    }
  }
  const model = numberOf(geokeys[7], "GeoKey model");
  const raster = numberOf(geokeys[11], "GeoKey raster type");
  const crsKey = numberOf(geokeys[12], "GeoKey CRS key");
  const epsgValue = numberOf(geokeys[15], "GeoKey EPSG code");
  if (geokeys[4] !== 1024n || geokeys[8] !== 1025n || ![1, 2].includes(raster)) {
    throw new Error("invalid Rumi GeoKey directory");
  }
  let epsg = null;
  if (model === 0) {
    if (crsKey !== 2048 || epsgValue !== 0) throw new Error("invalid undefined Rumi CRS");
    const identity = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1];
    if (matrix.some((value, index) => value !== identity[index])) {
      throw new Error("undefined Rumi georeferencing must use the identity transform");
    }
  } else if ((model === 1 && crsKey === 3072) || (model === 2 && crsKey === 2048)) {
    if (epsgValue < 1024 || epsgValue > 32766) throw new Error("Rumi EPSG code is out of range");
    const kind = epsgKind(epsgValue);
    if (kind === undefined) throw new Error(`EPSG:${epsgValue} is not a supported geographic or projected CRS`);
    if ((model === 1) !== (kind === "projected")) {
      throw new Error(`Rumi GeoKey model disagrees with EPSG:${epsgValue}`);
    }
    epsg = epsgValue;
  } else {
    throw new Error("Rumi GeoKey model and CRS key disagree");
  }

  const offsetEntry = entries.get(324);
  const countEntry = entries.get(325);
  if (offsetEntry.count === 0n || offsetEntry.count > MAX_U32 || countEntry.count !== offsetEntry.count) {
    throw new Error("invalid Rumi frame table lengths");
  }
  const frameCount = Number(offsetEntry.count);
  const present = Number(fields.bands > 1) + Number(fields.timeCount > 1);
  const validUnit = fields.frameUnit === 0
    || ((fields.frameUnit === 1 || fields.frameUnit === 2) && present === 1)
    || (fields.frameUnit >= 3 && fields.frameUnit <= 9 && present === 2);
  if (!validUnit) throw new Error("frameUnit does not fit the band and time dimensions");
  let expectedFrames = BigInt(Math.ceil(fields.width / fields.tileWidth))
    * BigInt(Math.ceil(fields.height / fields.tileHeight));
  if (fields.frameUnit === 0 || fields.frameUnit === 9) {
    expectedFrames *= BigInt(fields.bands) * BigInt(fields.timeCount);
  }
  if (offsetEntry.count !== expectedFrames) {
    throw new Error(`frame table has ${offsetEntry.count} entries, expected ${expectedFrames}`);
  }
  if (frameCount * 12 > maxMetadataBytes) throw new RangeError("frame index exceeds maxMetadataBytes");
  const offsetBytes = entryBytes(metadata, offsetEntry, maxMetadataBytes);
  const countBytes = entryBytes(metadata, countEntry, maxMetadataBytes);
  const offsetView = new DataView(offsetBytes.buffer, offsetBytes.byteOffset, offsetBytes.byteLength);
  const countView = new DataView(countBytes.buffer, countBytes.byteOffset, countBytes.byteLength);
  const counts = new Uint32Array(frameCount);
  const firstOffset = offsetView.getBigUint64(0, true);
  let running = firstOffset;
  for (let index = 0; index < frameCount; index += 1) {
    const offset = offsetView.getBigUint64(index * 8, true);
    const count = BigInt(countView.getUint32(index * 4, true));
    if (count === 0n || count > MAX_U32) throw new RangeError(`frame ${index} has an invalid byte count`);
    if (offset !== running) throw new Error(`frame ${index} is not contiguous`);
    if (running > MAX_U64 - count) throw new RangeError("frame offsets overflow uint64");
    counts[index] = Number(count);
    running += count;
  }
  if (firstOffset !== derivedBaseOffset(fields.bands, BigInt(frameCount))) {
    throw new Error("frame data does not start at the canonical offset");
  }
  const trailer = await readTrailer(
    source,
    running,
    fields.bands,
    fields.timeCount,
    maxMetadataBytes,
    maxTimeCoordinates,
  );
  return {
    header: makeExternalHeader(fields, counts),
    metadata: Object.freeze({
      transform: epsg === null
        ? null
        : Object.freeze([matrix[0], matrix[1], matrix[3], matrix[4], matrix[5], matrix[7]]),
      epsg,
      crs: epsg === null ? null : `EPSG:${epsg}`,
      pixelIsPoint: raster === 2,
      bands: trailer.bands,
      time: trailer.time,
      timeKind: trailer.timeKind,
    }),
  };
}
