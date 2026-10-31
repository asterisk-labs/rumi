import { readExact, sourceSize } from "./source.js";

const MAGIC = 0x4c494154;
const VERSION = 1;
const FIXED_SIZE = 22;
const INT64_MIN = -(1n << 63n);
const INT64_MAX = (1n << 63n) - 1n;

function floorDiv(value, divisor) {
  return value >= 0n
    ? value / divisor
    : -((-value + divisor - 1n) / divisor);
}

function roundedRatio(value, divisor) {
  return floorDiv(2n * value + divisor, 2n * divisor);
}

function bitLength(value) {
  let bits = 0;
  while (value !== 0n) {
    value >>= 1n;
    bits += 1;
  }
  return bits;
}

function packedValue(bytes, index, bits) {
  if (bits === 0) return 0n;
  const at = BigInt(index) * BigInt(bits);
  const first = Number(at >> 3n);
  const shift = Number(at & 7n);
  const count = Math.ceil((shift + bits) / 8);
  let value = 0n;
  for (let byte = 0; byte < count; byte += 1) {
    value |= BigInt(bytes[first + byte]) << BigInt(byte * 8);
  }
  return (value >> BigInt(shift)) & ((1n << BigInt(bits)) - 1n);
}

function residualOf(value) {
  return (value & 1n) === 0n ? value >> 1n : -((value >> 1n) + 1n);
}

function freezeTime(kind, seconds) {
  if (kind === "instant") return Object.freeze(seconds);
  return Object.freeze(Array.from(
    { length: seconds.length / 2 },
    (_, index) => Object.freeze([seconds[index * 2], seconds[index * 2 + 1]]),
  ));
}

export async function readTrailer(
  source,
  offset,
  bands,
  timeCount,
  maxMetadataBytes,
  maxTimeCoordinates,
) {
  const total = await sourceSize(source);
  if (offset > total) throw new RangeError("Rumi trailer starts past the end of the source");
  const length = total - offset;
  if (length > BigInt(maxMetadataBytes)) {
    throw new RangeError("Rumi trailer exceeds maxMetadataBytes");
  }
  const bytes = await readExact(source, offset, Number(length));
  let cursor = 0;
  function take(size) {
    if (!Number.isSafeInteger(size) || size < 0 || cursor > bytes.byteLength - size) {
      throw new RangeError("Rumi trailer exceeds maxMetadataBytes");
    }
    const value = bytes.subarray(cursor, cursor + size);
    cursor += size;
    return value;
  }

  const prefix = take(6);
  const prefixView = new DataView(prefix.buffer, prefix.byteOffset, prefix.byteLength);
  if (prefixView.getUint32(0, true) !== MAGIC) throw new Error("invalid Rumi trailer magic");
  if (prefixView.getUint16(4, true) !== VERSION) throw new Error("unsupported Rumi trailer version");

  const decoder = new TextDecoder("utf-8", { fatal: true, ignoreBOM: true });
  const texts = [];
  const unique = new Set();
  for (let band = 0; band < bands; band += 1) {
    const lengthBytes = take(2);
    const length = new DataView(
      lengthBytes.buffer,
      lengthBytes.byteOffset,
      lengthBytes.byteLength,
    ).getUint16(0, true);
    if (length === 0) throw new Error(`band ${band} has an empty description`);
    let text;
    try {
      text = decoder.decode(take(length));
    } catch {
      throw new Error(`band ${band} description is not valid UTF-8`);
    }
    if (text.includes("\0")) throw new Error(`band ${band} description contains NUL`);
    if (unique.has(text)) throw new Error(`band ${band} description is not unique`);
    unique.add(text);
    texts.push(text);
  }

  const fixed = take(FIXED_SIZE);
  const view = new DataView(fixed.buffer, fixed.byteOffset, fixed.byteLength);
  const timeType = view.getUint8(0);
  const timeBits = view.getUint8(1);
  if (timeType !== 1 && timeType !== 2) throw new Error("invalid Rumi time type");
  if (timeBits > 64) throw new Error("Rumi time_bits is greater than 64");
  const timeEpoch = view.getBigInt64(2, true);
  const timeStep = view.getBigInt64(10, true);
  const timeScale = view.getUint32(18, true);
  if (timeScale !== 1 && timeScale !== 86400) throw new Error("invalid Rumi time scale");

  const coordinateCount = timeCount * (timeType === 1 ? 2 : 1);
  if (!Number.isSafeInteger(coordinateCount)
      || coordinateCount > maxTimeCoordinates) {
    throw new RangeError("Rumi time coordinates exceed maxTimeCoordinates");
  }
  const packedLength = Math.ceil(coordinateCount * timeBits / 8);
  const packed = take(packedLength);
  if (cursor !== bytes.byteLength) throw new Error("Rumi file has bytes after its trailer");
  const spare = packedLength * 8 - coordinateCount * timeBits;
  if (spare !== 0 && (packed[packedLength - 1] >> (8 - spare)) !== 0) {
    throw new Error("Rumi time residual padding is not zero");
  }
  const coordinates = new Array(coordinateCount);
  let widest = 0;
  for (let index = 0; index < coordinateCount; index += 1) {
    const encoded = packedValue(packed, index, timeBits);
    widest = Math.max(widest, bitLength(encoded));
    const coordinate = timeEpoch + BigInt(index) * timeStep + residualOf(encoded);
    if (coordinate < INT64_MIN || coordinate > INT64_MAX) {
      throw new RangeError(`Rumi time coordinate ${index} overflows int64`);
    }
    coordinates[index] = coordinate;
  }
  if (widest !== timeBits) throw new Error("Rumi time_bits is not canonical");
  if (coordinates[0] !== timeEpoch) throw new Error("Rumi time_epoch is not canonical");
  const expectedStep = coordinateCount < 2
    ? 0n
    : roundedRatio(coordinates.at(-1) - coordinates[0], BigInt(coordinateCount - 1));
  if (timeStep !== expectedStep) throw new Error("Rumi time_step is not canonical");
  if (timeScale === 1 && coordinates.every((value) => value % 86400n === 0n)) {
    throw new Error("Rumi time_scale is not canonical");
  }

  if (timeType === 2) {
    for (let index = 1; index < coordinates.length; index += 1) {
      if (coordinates[index] < coordinates[index - 1]) {
        throw new Error(`Rumi instant ${index} goes backwards`);
      }
    }
  } else {
    for (let index = 0; index < timeCount; index += 1) {
      const start = coordinates[index * 2];
      const end = coordinates[index * 2 + 1];
      if (start >= end) throw new Error(`Rumi interval ${index} is empty or reversed`);
      if (index > 0 && coordinates[index * 2 - 1] > start) {
        throw new Error(`Rumi interval ${index} overlaps the previous interval`);
      }
    }
  }

  const scale = BigInt(timeScale);
  const seconds = coordinates.map((value, index) => {
    const second = value * scale;
    if (second < INT64_MIN || second > INT64_MAX) {
      throw new RangeError(`Rumi time coordinate ${index} in seconds overflows int64`);
    }
    return second;
  });
  const timeKind = timeType === 2 ? "instant" : "interval";
  return Object.freeze({
    bands: Object.freeze(texts),
    time: freezeTime(timeKind, seconds),
    timeKind,
    end: offset + BigInt(cursor),
  });
}
