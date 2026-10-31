import { storageView } from "./dtypes.js";
import { externalHeaderFromFile } from "./file_header.js";
import { parseHeader, unitPixelStep, unitPlaneOffset } from "./header.js";
import { readExact, sourceFrom } from "./source.js";

const DEFAULT_MAX_OUTPUT_BYTES = 1 << 30;
const DEFAULT_MAX_METADATA_BYTES = 64 * 1024 * 1024;
const DEFAULT_MAX_TIME_COORDINATES = 1_000_000;
const MAX_INDEX_FRAMES = Math.floor((64 * 1024 * 1024) / 12);
const MAX_PARALLEL_READS = 8;
let sharedDecoder;

function checkedProduct(values, label) {
  let result = 1;
  for (const value of values) {
    if (!Number.isSafeInteger(value) || value < 0 || (value !== 0 && result > Number.MAX_SAFE_INTEGER / value)) {
      throw new RangeError(`${label} exceeds JavaScript's safe integer range`);
    }
    result *= value;
  }
  return result;
}

function validateLimit(value, label) {
  if (!Number.isSafeInteger(value) || value <= 0) {
    throw new RangeError(`${label} must be a positive safe integer`);
  }
}

async function forEachConcurrent(values, callback) {
  let next = 0;
  let stopped = false;
  async function worker() {
    while (!stopped) {
      const index = next;
      next += 1;
      if (index >= values.length) return;
      try {
        await callback(values[index]);
      } catch (error) {
        stopped = true;
        throw error;
      }
    }
  }
  const count = Math.min(MAX_PARALLEL_READS, values.length);
  await Promise.all(Array.from({ length: count }, worker));
}

function selection(value, extent, label) {
  if (value === undefined) {
    return { length: extent, uniqueLength: extent, at: (index) => index };
  }
  if (!Array.isArray(value) && !ArrayBuffer.isView(value)) {
    throw new TypeError(`${label} must be an array of zero-based indices`);
  }
  const out = Array.from(value);
  if (out.length === 0) throw new RangeError(`${label} cannot be empty`);
  for (const index of out) {
    if (!Number.isSafeInteger(index) || index < 0 || index >= extent) {
      throw new RangeError(`${label} index ${index} is out of range`);
    }
  }
  return {
    length: out.length,
    uniqueLength: new Set(out).size,
    at: (index) => out[index],
  };
}

function windowOf(value, header) {
  if (value === undefined) return [0, 0, header.height, header.width];
  if (!Array.isArray(value) || value.length !== 4) {
    throw new TypeError("window must be [y, x, height, width]");
  }
  const [y, x, height, width] = value;
  for (const part of value) {
    if (!Number.isSafeInteger(part) || part < 0) {
      throw new RangeError("window values must be non-negative safe integers");
    }
  }
  if (height === 0 || width === 0) throw new RangeError("window cannot be empty");
  if (y > header.height - height || x > header.width - width) {
    throw new RangeError("window extends past the image");
  }
  return [y, x, height, width];
}

function requestOf(options, header) {
  const times = selection(options.time, header.timeCount, "time");
  const bands = selection(options.bands, header.bands, "bands");
  if (header.timeCount === 1 && times.length !== 1) {
    throw new RangeError("an Image time selection must contain one index");
  }
  const window = windowOf(options.window, header);
  return { times, bands, window };
}

function requestedFrameCount(header, request) {
  const [y, x, height, width] = request.window;
  const tileRows = Math.ceil((y + height) / header.tileHeight) - Math.floor(y / header.tileHeight);
  const tileColumns = Math.ceil((x + width) / header.tileWidth) - Math.floor(x / header.tileWidth);
  return checkedProduct([
    tileRows,
    tileColumns,
    header.frameAxes.includes("band") ? 1 : request.bands.uniqueLength,
    header.frameAxes.includes("time") ? 1 : request.times.uniqueLength,
  ], "planned frame count");
}

function frameGroups(header, request) {
  const [y, x, height, width] = request.window;
  const ty0 = Math.floor(y / header.tileHeight);
  const tx0 = Math.floor(x / header.tileWidth);
  const ty1 = Math.ceil((y + height) / header.tileHeight);
  const tx1 = Math.ceil((x + width) / header.tileWidth);
  const groups = new Map();

  for (let ty = ty0; ty < ty1; ty += 1) {
    for (let tx = tx0; tx < tx1; tx += 1) {
      const spatial = ty * header.tilesAcross + tx;
      for (let outTime = 0; outTime < request.times.length; outTime += 1) {
        const time = request.times.at(outTime);
        for (let outBand = 0; outBand < request.bands.length; outBand += 1) {
          const band = request.bands.at(outBand);
          const frameIndex = header.frameIndex(spatial, band, time);
          let group = groups.get(frameIndex);
          if (!group) {
            group = { frameIndex, ty, tx, planes: [] };
            groups.set(frameIndex, group);
          }
          group.planes.push(time, band, outTime, outBand);
        }
      }
    }
  }
  return [...groups.values()].sort((a, b) => a.frameIndex - b.frameIndex);
}

function decoderFunction(decoder) {
  if (typeof decoder === "function") return decoder;
  if (decoder && typeof decoder.decompress === "function") {
    return (frame, options) => decoder.decompress(frame, options);
  }
  throw new TypeError("decoder must be a function or expose decompress()");
}

function defaultDecoder(options) {
  if (options !== undefined) {
    return import("@asterisk-labs/geozl").then(({ createGeoZL }) => createGeoZL(options));
  }
  sharedDecoder ??= import("@asterisk-labs/geozl")
    .then(({ createGeoZL }) => createGeoZL());
  return sharedDecoder;
}

async function decodedBytes(decode, compressed, expected, verify) {
  const value = await decode(compressed, { verify, maxOutputSize: expected });
  let bytes;
  if (value instanceof Uint8Array) {
    bytes = value;
  } else if (value instanceof ArrayBuffer
      || (typeof SharedArrayBuffer !== "undefined" && value instanceof SharedArrayBuffer)) {
    bytes = new Uint8Array(value);
  } else if (ArrayBuffer.isView(value)) {
    bytes = new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
  } else {
    throw new TypeError("decoder must return bytes or a typed array");
  }
  if (bytes.byteLength !== expected) {
    throw new RangeError(`decoded frame has ${bytes.byteLength} bytes, expected ${expected}`);
  }
  return bytes;
}

function validateDecodedSamples(bytes, dtype) {
  if (dtype.bits !== 1) return;
  const invalid = bytes.findIndex((value) => value !== 0 && value !== 1);
  if (invalid !== -1) {
    throw new Error(`decoded bool byte ${invalid} is neither 0 nor 1`);
  }
}

function publicInfo(header, metadata) {
  const axes = header.timeCount === 1 ? ["band", "y", "x"] : ["time", "band", "y", "x"];
  const shape = header.timeCount === 1
    ? [header.bands, header.height, header.width]
    : [header.timeCount, header.bands, header.height, header.width];
  return Object.freeze({
    shape: Object.freeze(shape),
    axes: Object.freeze(axes),
    dtype: header.dtype,
    tileShape: Object.freeze([header.tileHeight, header.tileWidth]),
    gridShape: Object.freeze([header.tilesDown, header.tilesAcross]),
    frameUnit: header.frameUnit,
    frameLayout: header.frameLayout,
    frameCount: header.frameCount,
    transform: metadata.transform,
    epsg: metadata.epsg,
    crs: metadata.crs,
    pixelIsPoint: metadata.pixelIsPoint,
    bands: metadata.bands,
    time: metadata.time,
    timeKind: metadata.timeKind,
  });
}

export class RumiDataset {
  constructor(source, header, decoder, maxOutputBytes, metadata) {
    this.source = source;
    this.header = header;
    this.decoder = decoder;
    this.maxOutputBytes = maxOutputBytes;
    this.metadata = Object.freeze({
      transform: metadata.transform ?? null,
      epsg: metadata.epsg ?? null,
      crs: metadata.crs ?? null,
      pixelIsPoint: metadata.pixelIsPoint ?? false,
      bands: metadata.bands ?? null,
      time: metadata.time ?? null,
      timeKind: metadata.timeKind ?? null,
    });
  }

  info() {
    return publicInfo(this.header, this.metadata);
  }

  planRanges(options = {}) {
    const request = requestOf(options, this.header);
    if (requestedFrameCount(this.header, request) > MAX_INDEX_FRAMES) {
      throw new RangeError("planned ranges exceed 64 MiB");
    }
    const groups = frameGroups(this.header, request);
    return groups.map(({ frameIndex }) => Object.freeze({
      frameIndex,
      offset: this.header.frameOffset(frameIndex),
      length: this.header.frameByteCount(frameIndex),
    }));
  }

  async read(options = {}) {
    const request = requestOf(options, this.header);
    const [, , outHeight, outWidth] = request.window;
    const outputBytes = checkedProduct([
      request.times.length,
      request.bands.length,
      outHeight,
      outWidth,
      this.header.dtype.bytes,
    ], "output size");
    if (outputBytes > this.maxOutputBytes) throw new RangeError("read exceeds maxOutputBytes");
    if (requestedFrameCount(this.header, request) > MAX_INDEX_FRAMES) {
      throw new RangeError("planned ranges exceed 64 MiB");
    }
    const output = new Uint8Array(outputBytes);
    const groups = frameGroups(this.header, request);
    const decode = decoderFunction(await this.decoder);
    const pixelStep = unitPixelStep(this.header);
    const bytesPerSample = this.header.dtype.bytes;
    const [windowY, windowX] = request.window;

    await forEachConcurrent(groups, async (group) => {
      const tileX = group.tx * this.header.tileWidth;
      const tileY = group.ty * this.header.tileHeight;
      const frameWidth = Math.min(this.header.tileWidth, this.header.width - tileX);
      const frameHeight = Math.min(this.header.tileHeight, this.header.height - tileY);
      const frameBytes = checkedProduct([
        frameWidth,
        frameHeight,
        bytesPerSample,
        this.header.frameAxes.includes("band") ? this.header.bands : 1,
        this.header.frameAxes.includes("time") ? this.header.timeCount : 1,
      ], "decoded frame size");
      if (frameBytes > this.header.maxDecodedFrame) throw new RangeError("decoded frame exceeds its header bound");

      const compressed = await readExact(
        this.source,
        this.header.frameOffset(group.frameIndex),
        this.header.frameByteCount(group.frameIndex),
      );
      const decoded = await decodedBytes(decode, compressed, frameBytes, options.verify !== false);
      validateDecodedSamples(decoded, this.header.dtype);

      const ix0 = Math.max(tileX, windowX);
      const iy0 = Math.max(tileY, windowY);
      const ix1 = Math.min(tileX + frameWidth, windowX + outWidth);
      const iy1 = Math.min(tileY + frameHeight, windowY + outHeight);
      const copyWidth = ix1 - ix0;
      const srcX = ix0 - tileX;
      const srcY = iy0 - tileY;
      const dstX = ix0 - windowX;
      const dstY = iy0 - windowY;

      for (let at = 0; at < group.planes.length; at += 4) {
        const time = group.planes[at];
        const band = group.planes[at + 1];
        const outTime = group.planes[at + 2];
        const outBand = group.planes[at + 3];
        const planeOffset = unitPlaneOffset(
          this.header,
          band,
          time,
          frameHeight,
          frameWidth,
        );
        for (let row = 0; row < iy1 - iy0; row += 1) {
          const srcSample = planeOffset + (srcY + row) * frameWidth * pixelStep + srcX * pixelStep;
          const dstSample = ((outTime * request.bands.length + outBand)
            * outHeight * outWidth) + (dstY + row) * outWidth + dstX;
          if (pixelStep === 1) {
            output.set(
              decoded.subarray(srcSample * bytesPerSample, (srcSample + copyWidth) * bytesPerSample),
              dstSample * bytesPerSample,
            );
          } else {
            for (let column = 0; column < copyWidth; column += 1) {
              const src = (srcSample + column * pixelStep) * bytesPerSample;
              const dst = (dstSample + column) * bytesPerSample;
              output.set(decoded.subarray(src, src + bytesPerSample), dst);
            }
          }
        }
      }
    });

    const shape = this.header.timeCount === 1
      ? [request.bands.length, outHeight, outWidth]
      : [request.times.length, request.bands.length, outHeight, outWidth];
    const axes = this.header.timeCount === 1 ? ["band", "y", "x"] : ["time", "band", "y", "x"];
    return Object.freeze({
      data: storageView(output, this.header.dtype),
      bytes: output,
      shape: Object.freeze(shape),
      axes: Object.freeze(axes),
      dtype: this.header.dtype,
    });
  }
}

export async function openRumi(value, options = {}) {
  const maxFrameBytes = options.maxFrameBytes ?? (1 << 30);
  const maxOutputBytes = options.maxOutputBytes ?? DEFAULT_MAX_OUTPUT_BYTES;
  const maxMetadataBytes = options.maxMetadataBytes ?? DEFAULT_MAX_METADATA_BYTES;
  const maxTimeCoordinates = options.maxTimeCoordinates ?? DEFAULT_MAX_TIME_COORDINATES;
  validateLimit(maxFrameBytes, "maxFrameBytes");
  validateLimit(maxOutputBytes, "maxOutputBytes");
  validateLimit(maxMetadataBytes, "maxMetadataBytes");
  validateLimit(maxTimeCoordinates, "maxTimeCoordinates");
  const source = sourceFrom(value, options);
  const rebuilt = await externalHeaderFromFile(source, {
    maxMetadataBytes,
    maxTimeCoordinates,
  });
  const header = parseHeader(rebuilt.header, { maxFrameBytes });
  const decoder = options.decoder === undefined
    ? defaultDecoder(options.geozl)
    : Promise.resolve(options.decoder);
  return new RumiDataset(source, header, decoder, maxOutputBytes, rebuilt.metadata);
}
