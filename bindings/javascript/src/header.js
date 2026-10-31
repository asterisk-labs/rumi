import { dtypeFromSample } from "./dtypes.js";

const MAGIC = 0x45564f4c;
const VERSION = 1;
const HEADER_SIZE = 32;
const MAX_U32 = 0xffffffff;
const MAX_I32 = 0x7fffffff;
const MAX_U64 = (1n << 64n) - 1n;
const MAX_INDEX_BYTES = 64 * 1024 * 1024;
const INDEX_BYTES_PER_FRAME = 12;

const AXIS_BAND = "band";
const AXIS_TIME = "time";
const AXIS_H = "y";
const AXIS_W = "x";

const units = [
  { axes: [AXIS_H, AXIS_W], timeFirst: false },
  { axes: ["one", AXIS_H, AXIS_W], timeFirst: false },
  { axes: [AXIS_H, AXIS_W, "one"], timeFirst: false },
  { axes: [AXIS_BAND, AXIS_TIME, AXIS_H, AXIS_W], timeFirst: false },
  { axes: [AXIS_TIME, AXIS_BAND, AXIS_H, AXIS_W], timeFirst: false },
  { axes: [AXIS_BAND, AXIS_H, AXIS_W, AXIS_TIME], timeFirst: false },
  { axes: [AXIS_TIME, AXIS_H, AXIS_W, AXIS_BAND], timeFirst: false },
  { axes: [AXIS_H, AXIS_W, AXIS_BAND, AXIS_TIME], timeFirst: false },
  { axes: [AXIS_H, AXIS_W, AXIS_TIME, AXIS_BAND], timeFirst: false },
  { axes: [AXIS_H, AXIS_W], timeFirst: true },
];

function asBytes(value) {
  if (value instanceof Uint8Array) return value;
  if (value instanceof ArrayBuffer
      || (typeof SharedArrayBuffer !== "undefined" && value instanceof SharedArrayBuffer)) {
    return new Uint8Array(value);
  }
  if (ArrayBuffer.isView(value)) {
    return new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
  }
  throw new TypeError("header must be an ArrayBuffer or typed array");
}

function checkedNumber(value, label) {
  if (value > BigInt(Number.MAX_SAFE_INTEGER)) {
    throw new RangeError(`${label} exceeds JavaScript's safe integer range`);
  }
  return Number(value);
}

function externalValueBytes(count, bytesPerValue) {
  const bytes = BigInt(count) * BigInt(bytesPerValue);
  return bytes > 8n ? bytes + (bytes & 1n) : 0n;
}

export function derivedBaseOffset(bands, frames) {
  return 16n + 276n
    + externalValueBytes(16, 8)
    + externalValueBytes(16, 2)
    + externalValueBytes(bands, 2)
    + externalValueBytes(bands, 2)
    + externalValueBytes(frames, 8)
    + externalValueBytes(frames, 4);
}

export function makeExternalHeader(fields, counts) {
  if (counts.length === 0) throw new RangeError("a Rumi file must contain frames");
  let countMin = counts[0];
  for (const count of counts) {
    if (!Number.isInteger(count) || count <= 0 || count > MAX_U32) {
      throw new RangeError("frame byte counts must be positive uint32 values");
    }
    countMin = Math.min(countMin, count);
  }
  let residualOr = 0n;
  for (const count of counts) residualOr |= BigInt(count - countMin);
  const countBits = bitWidth(residualOr);
  const packedLength = Math.ceil(counts.length * countBits / 8);
  const bytes = new Uint8Array(HEADER_SIZE + packedLength);
  const view = new DataView(bytes.buffer);
  view.setUint32(0, MAGIC, true);
  view.setUint16(4, VERSION, true);
  view.setUint32(6, fields.width, true);
  view.setUint32(10, fields.height, true);
  view.setUint32(14, fields.timeCount, true);
  view.setUint16(18, fields.tileWidth, true);
  view.setUint16(20, fields.tileHeight, true);
  view.setUint16(22, fields.bands, true);
  view.setUint8(24, fields.bits);
  view.setUint8(25, fields.sampleFormat);
  view.setUint8(26, fields.frameUnit);
  view.setUint32(27, countMin, true);
  view.setUint8(31, countBits);

  for (let index = 0; index < counts.length; index += 1) {
    const at = BigInt(index * countBits);
    const first = Number(at >> 3n);
    const word = BigInt(counts[index] - countMin) << (at & 7n);
    for (let byte = 0; byte < 5 && first + byte < packedLength; byte += 1) {
      bytes[HEADER_SIZE + first + byte] |= Number((word >> BigInt(byte * 8)) & 0xffn);
    }
  }
  return bytes;
}

function resolvedAxes(unit, bands, times) {
  const row = units[unit];
  if (!row) return undefined;
  const present = Number(bands > 1) + Number(times > 1);
  if (unit !== 0 && unit !== 1 && unit !== 2 && present !== 2) return undefined;
  if ((unit === 1 || unit === 2) && present !== 1) return undefined;
  if (bands === 0 || times === 0) return undefined;
  const only = bands > 1 ? AXIS_BAND : AXIS_TIME;
  return row.axes.map((axis) => axis === "one" ? only : axis);
}

function product(values, label) {
  let out = 1n;
  for (const value of values) {
    out *= BigInt(value);
    if (out > MAX_U64) throw new RangeError(`${label} overflows uint64`);
  }
  return out;
}

function bitWidth(value) {
  let bits = 0;
  while (value !== 0n) {
    value >>= 1n;
    bits += 1;
  }
  return bits;
}

function unpackResidual(packed, index, width) {
  if (width === 0) return 0n;
  const at = BigInt(index) * BigInt(width);
  const first = Number(at >> 3n);
  const shift = Number(at & 7n);
  let word = 0n;
  const available = Math.min(5, packed.length - first);
  for (let i = 0; i < available; i += 1) {
    word |= BigInt(packed[first + i]) << BigInt(8 * i);
  }
  return (word >> BigInt(shift)) & ((1n << BigInt(width)) - 1n);
}

function frameIndexOf(header, spatial, band, time) {
  let index = spatial;
  const indexedBand = !header.frameAxes.includes(AXIS_BAND);
  const indexedTime = !header.frameAxes.includes(AXIS_TIME);
  const order = header.timeFirst ? [AXIS_TIME, AXIS_BAND] : [AXIS_BAND, AXIS_TIME];
  for (const axis of order) {
    if (axis === AXIS_BAND && indexedBand) index = index * header.bands + band;
    if (axis === AXIS_TIME && indexedTime) index = index * header.timeCount + time;
  }
  return index;
}

export function parseHeader(value, { maxFrameBytes = 1 << 30 } = {}) {
  const bytes = asBytes(value);
  if (!Number.isSafeInteger(maxFrameBytes) || maxFrameBytes <= 0) {
    throw new RangeError("maxFrameBytes must be a positive safe integer");
  }
  if (bytes.byteLength < HEADER_SIZE) throw new RangeError("header is shorter than 32 bytes");

  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  if (view.getUint32(0, true) !== MAGIC) throw new Error("bad Rumi header magic");
  if (view.getUint16(4, true) !== VERSION) throw new Error("unsupported Rumi header version");

  const width = view.getUint32(6, true);
  const height = view.getUint32(10, true);
  const timeCount = view.getUint32(14, true);
  const tileWidth = view.getUint16(18, true);
  const tileHeight = view.getUint16(20, true);
  const bands = view.getUint16(22, true);
  const bits = view.getUint8(24);
  const sampleFormat = view.getUint8(25);
  const frameUnit = view.getUint8(26);
  const countMin = view.getUint32(27, true);
  const countBits = view.getUint8(31);

  const dtype = dtypeFromSample(sampleFormat, bits);
  if (!dtype) throw new Error(`unsupported sample format ${sampleFormat}/${bits}`);
  if (width === 0 || height === 0 || timeCount === 0 || tileWidth === 0
      || tileHeight === 0 || bands === 0 || width > MAX_I32 || height > MAX_I32) {
    throw new RangeError("invalid Rumi dimensions");
  }
  const frameAxes = resolvedAxes(frameUnit, bands, timeCount);
  if (!frameAxes) throw new Error("frameUnit does not fit the band and time dimensions");
  if (countBits > 32) throw new RangeError("countBits is greater than 32");

  const tilesAcross = Math.ceil(width / tileWidth);
  const tilesDown = Math.ceil(height / tileHeight);
  const holdsBand = frameAxes.includes(AXIS_BAND);
  const holdsTime = frameAxes.includes(AXIS_TIME);
  const frameCountBig = product([
    tilesAcross,
    tilesDown,
    holdsBand ? 1 : bands,
    holdsTime ? 1 : timeCount,
  ], "frame count");
  if (frameCountBig > BigInt(MAX_U32)) throw new RangeError("frame count exceeds uint32");
  const frameCount = Number(frameCountBig);

  const packedLengthBig = (frameCountBig * BigInt(countBits) + 7n) >> 3n;
  const packedLength = checkedNumber(packedLengthBig, "packed frame index");
  if (bytes.byteLength !== HEADER_SIZE + packedLength) {
    throw new RangeError("header size does not match the frame index");
  }

  const nominalWidth = Math.min(width, tileWidth);
  const nominalHeight = Math.min(height, tileHeight);
  const maxDecodedFrame = product([
    nominalWidth,
    nominalHeight,
    dtype.bytes,
    holdsBand ? bands : 1,
    holdsTime ? timeCount : 1,
  ], "decoded frame size");
  if (maxDecodedFrame > BigInt(maxFrameBytes)) {
    throw new RangeError("decoded frame exceeds maxFrameBytes");
  }

  const baseFrameOffset = derivedBaseOffset(bands, frameCountBig);
  const tooLargeToExpand = frameCount > Math.floor(MAX_INDEX_BYTES / INDEX_BYTES_PER_FRAME);
  if (tooLargeToExpand && countBits !== 0) {
    throw new RangeError("expanded frame index exceeds 64 MiB");
  }
  if (countBits === 0 && countMin === 0) throw new RangeError("frame byte count is zero");

  let frameByteCounts;
  let frameOffsets;
  if (countBits !== 0 || !tooLargeToExpand) {
    frameByteCounts = new Uint32Array(frameCount);
    frameOffsets = new BigUint64Array(frameCount);
    const packed = bytes.subarray(HEADER_SIZE);
    let offset = baseFrameOffset;
    let seenLow;
    let seenAny = 0n;

    for (let i = 0; i < frameCount; i += 1) {
      const residual = unpackResidual(packed, i, countBits);
      seenLow = seenLow === undefined || residual < seenLow ? residual : seenLow;
      seenAny |= residual;
      const count = BigInt(countMin) + residual;
      if (count === 0n) throw new RangeError("frame byte count is zero");
      if (count > BigInt(MAX_U32)) throw new RangeError("frame byte count exceeds uint32");
      if (offset > MAX_U64 - count) throw new RangeError("frame offsets overflow uint64");
      frameByteCounts[i] = Number(count);
      frameOffsets[i] = offset;
      offset += count;
    }

    if (seenLow !== 0n || bitWidth(seenAny) !== countBits) {
      throw new Error("frame byte counts are not packed canonically");
    }
    const spare = packedLength * 8 - frameCount * countBits;
    if (spare !== 0 && (packed[packedLength - 1] >> (8 - spare)) !== 0) {
      throw new Error("frame byte count padding is not zero");
    }
  } else if (baseFrameOffset > MAX_U64 - frameCountBig * BigInt(countMin)) {
    throw new RangeError("frame offsets overflow uint64");
  }

  const header = {
    width,
    height,
    timeCount,
    tileWidth,
    tileHeight,
    bands,
    dtype,
    frameUnit,
    frameAxes: Object.freeze(frameAxes),
    frameLayout: frameAxes.map((axis) => ({ band: "b", time: "t", y: "h", x: "w" })[axis]).join(" "),
    timeFirst: units[frameUnit].timeFirst,
    tilesAcross,
    tilesDown,
    frameCount,
    maxDecodedFrame: Number(maxDecodedFrame),
    baseFrameOffset,
    countMin,
    countBits,
    frameOffset(index) {
      if (!Number.isInteger(index) || index < 0 || index >= frameCount) {
        throw new RangeError("frame index is out of range");
      }
      if (frameOffsets) return frameOffsets[index];
      return baseFrameOffset + BigInt(index) * BigInt(countMin);
    },
    frameByteCount(index) {
      if (!Number.isInteger(index) || index < 0 || index >= frameCount) {
        throw new RangeError("frame index is out of range");
      }
      return frameByteCounts ? frameByteCounts[index] : countMin;
    },
    frameIndex(spatial, band, time) {
      return frameIndexOf(header, spatial, band, time);
    },
  };
  return Object.freeze(header);
}

export function unitPixelStep(header) {
  let step = 1;
  let afterWidth = false;
  for (const axis of header.frameAxes) {
    if (axis === AXIS_W) {
      afterWidth = true;
    } else if (afterWidth && axis === AXIS_BAND) {
      step *= header.bands;
    } else if (afterWidth && axis === AXIS_TIME) {
      step *= header.timeCount;
    }
  }
  return step;
}

export function unitPlaneOffset(header, band, time, height, width) {
  let stride = 1;
  let offset = 0;
  for (let i = header.frameAxes.length - 1; i >= 0; i -= 1) {
    const axis = header.frameAxes[i];
    if (axis === AXIS_BAND) {
      offset += band * stride;
      stride *= header.bands;
    } else if (axis === AXIS_TIME) {
      offset += time * stride;
      stride *= header.timeCount;
    } else {
      stride *= axis === AXIS_H ? height : width;
    }
  }
  return offset;
}
