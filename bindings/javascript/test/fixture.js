const UNIT_AXES = [
  ["y", "x"],
  ["one", "y", "x"],
  ["y", "x", "one"],
  ["band", "time", "y", "x"],
  ["time", "band", "y", "x"],
  ["band", "y", "x", "time"],
  ["time", "y", "x", "band"],
  ["y", "x", "band", "time"],
  ["y", "x", "time", "band"],
  ["y", "x"],
];

function externalBytes(count, width) {
  const size = BigInt(count) * BigInt(width);
  return size > 8n ? Number(size + (size & 1n)) : 0;
}

export function baseOffset(bands, frames) {
  return 16 + 276 + externalBytes(16, 8) + externalBytes(16, 2)
    + externalBytes(bands, 2) * 2 + externalBytes(frames, 8)
    + externalBytes(frames, 4);
}

function bitWidth(value) {
  let bits = 0;
  while (value !== 0) {
    value >>>= 1;
    bits += 1;
  }
  return bits;
}

export function makeHeader({
  width,
  height,
  times,
  tileWidth,
  tileHeight,
  bands,
  frameUnit,
  counts,
}) {
  const countMin = Math.min(...counts);
  let any = 0;
  for (const count of counts) any |= count - countMin;
  const countBits = bitWidth(any);
  const packedLength = Math.ceil(counts.length * countBits / 8);
  const bytes = new Uint8Array(32 + packedLength);
  const view = new DataView(bytes.buffer);
  view.setUint32(0, 0x45564f4c, true);
  view.setUint16(4, 1, true);
  view.setUint32(6, width, true);
  view.setUint32(10, height, true);
  view.setUint32(14, times, true);
  view.setUint16(18, tileWidth, true);
  view.setUint16(20, tileHeight, true);
  view.setUint16(22, bands, true);
  view.setUint8(24, 16);
  view.setUint8(25, 1);
  view.setUint8(26, frameUnit);
  view.setUint32(27, countMin, true);
  view.setUint8(31, countBits);

  for (let i = 0; i < counts.length; i += 1) {
    const residual = BigInt(counts[i] - countMin);
    const at = BigInt(i * countBits);
    const first = Number(at >> 3n);
    const shifted = residual << (at & 7n);
    for (let byte = 0; byte < 5 && first + byte < packedLength; byte += 1) {
      bytes[32 + first + byte] |= Number((shifted >> BigInt(8 * byte)) & 0xffn);
    }
  }
  return bytes;
}

function sampleValue(time, band, y, x) {
  return time * 1000 + band * 100 + y * 10 + x;
}

function encodeFrame(axes, extents, fixed) {
  const samples = extents.reduce((size, extent) => size * extent, 1);
  const bytes = new Uint8Array(samples * 2);
  const view = new DataView(bytes.buffer);
  let index = 0;

  function walk(depth, coordinates) {
    if (depth === axes.length) {
      const time = coordinates.time ?? fixed.time ?? 0;
      const band = coordinates.band ?? fixed.band ?? 0;
      const y = coordinates.y;
      const x = coordinates.x;
      view.setUint16(index * 2, sampleValue(time, band, fixed.tileY + y, fixed.tileX + x), true);
      index += 1;
      return;
    }
    const axis = axes[depth];
    for (let value = 0; value < extents[depth]; value += 1) {
      coordinates[axis] = value;
      walk(depth + 1, coordinates);
    }
  }
  walk(0, {});
  return bytes;
}

function indexCombinations(frameUnit, holdsBand, holdsTime, bands, times) {
  const order = frameUnit === 9 ? ["time", "band"] : ["band", "time"];
  const result = [];
  function walk(depth, fixed) {
    if (depth === order.length) {
      result.push(fixed);
      return;
    }
    const axis = order[depth];
    const indexed = axis === "band" ? !holdsBand : !holdsTime;
    if (!indexed) {
      walk(depth + 1, fixed);
      return;
    }
    const extent = axis === "band" ? bands : times;
    for (let value = 0; value < extent; value += 1) {
      walk(depth + 1, { ...fixed, [axis]: value });
    }
  }
  walk(0, {});
  return result;
}

export function makeRumi({
  width = 4,
  height = 3,
  times = 2,
  bands = 3,
  tileWidth = 3,
  tileHeight = 2,
  frameUnit = 3,
} = {}) {
  const only = bands > 1 ? "band" : "time";
  const axes = UNIT_AXES[frameUnit].map((axis) => axis === "one" ? only : axis);
  const holdsBand = axes.includes("band");
  const holdsTime = axes.includes("time");
  const across = Math.ceil(width / tileWidth);
  const down = Math.ceil(height / tileHeight);
  const frames = [];

  for (let ty = 0; ty < down; ty += 1) {
    for (let tx = 0; tx < across; tx += 1) {
      const frameWidth = Math.min(tileWidth, width - tx * tileWidth);
      const frameHeight = Math.min(tileHeight, height - ty * tileHeight);
      const extent = { band: bands, time: times, y: frameHeight, x: frameWidth };
      const extents = axes.map((axis) => extent[axis]);
      for (const fixed of indexCombinations(frameUnit, holdsBand, holdsTime, bands, times)) {
        frames.push(encodeFrame(axes, extents, {
          ...fixed,
          tileY: ty * tileHeight,
          tileX: tx * tileWidth,
        }));
      }
    }
  }

  const counts = frames.map((frame) => frame.byteLength);
  const header = makeHeader({
    width,
    height,
    times,
    tileWidth,
    tileHeight,
    bands,
    frameUnit,
    counts,
  });
  const base = baseOffset(bands, frames.length);
  const size = base + counts.reduce((total, count) => total + count, 0);
  const file = new Uint8Array(size);
  file.set(header);
  let offset = base;
  for (const frame of frames) {
    file.set(frame, offset);
    offset += frame.byteLength;
  }
  return { file, header, frames, sampleValue };
}

function integerBytes(values, width) {
  const bytes = new Uint8Array(values.length * width);
  const view = new DataView(bytes.buffer);
  values.forEach((value, index) => {
    if (width === 2) view.setUint16(index * width, value, true);
    else if (width === 4) view.setUint32(index * width, value, true);
    else view.setBigUint64(index * width, BigInt(value), true);
  });
  return bytes;
}

function doubleBytes(values) {
  const bytes = new Uint8Array(values.length * 8);
  const view = new DataView(bytes.buffer);
  values.forEach((value, index) => view.setFloat64(index * 8, value, true));
  return bytes;
}

function floorDiv(value, divisor) {
  return value >= 0n
    ? value / divisor
    : -((-value + divisor - 1n) / divisor);
}

function roundedRatio(value, divisor) {
  return floorDiv(2n * value + divisor, 2n * divisor);
}

function bigBitLength(value) {
  let bits = 0;
  while (value !== 0n) {
    value >>= 1n;
    bits += 1;
  }
  return bits;
}

function makeTrailer(bandTexts, timeKind, timeCoordinates) {
  const encoder = new TextEncoder();
  const encodedTexts = bandTexts.map((text) => encoder.encode(text));
  const scale = timeCoordinates.every((value) => value % 86400n === 0n) ? 86400n : 1n;
  const coordinates = timeCoordinates.map((value) => value / scale);
  const step = coordinates.length < 2
    ? 0n
    : roundedRatio(coordinates.at(-1) - coordinates[0], BigInt(coordinates.length - 1));
  const packed = coordinates.map((value, index) => {
    const residual = value - (coordinates[0] + BigInt(index) * step);
    return residual >= 0n ? residual * 2n : -residual * 2n - 1n;
  });
  const timeBits = packed.reduce((bits, value) => Math.max(bits, bigBitLength(value)), 0);
  const packedLength = Math.ceil(packed.length * timeBits / 8);
  const textLength = encodedTexts.reduce((size, text) => size + 2 + text.length, 0);
  const bytes = new Uint8Array(6 + textLength + 22 + packedLength);
  const view = new DataView(bytes.buffer);
  bytes.set([0x54, 0x41, 0x49, 0x4c]);
  view.setUint16(4, 1, true);
  let cursor = 6;
  for (const text of encodedTexts) {
    view.setUint16(cursor, text.length, true);
    cursor += 2;
    bytes.set(text, cursor);
    cursor += text.length;
  }
  view.setUint8(cursor, timeKind === "interval" ? 1 : 2);
  view.setUint8(cursor + 1, timeBits);
  view.setBigInt64(cursor + 2, coordinates[0], true);
  view.setBigInt64(cursor + 10, step, true);
  view.setUint32(cursor + 18, Number(scale), true);
  cursor += 22;
  for (let index = 0; index < packed.length; index += 1) {
    const at = BigInt(index * timeBits);
    const first = Number(at >> 3n);
    const shifted = packed[index] << (at & 7n);
    const count = Math.ceil((Number(at & 7n) + timeBits) / 8);
    for (let byte = 0; byte < count; byte += 1) {
      bytes[cursor + first + byte] |= Number((shifted >> BigInt(byte * 8)) & 0xffn);
    }
  }
  return bytes;
}

export function makeFileRumi(options = {}) {
  const fixture = makeRumi(options);
  const {
    width = 4,
    height = 3,
    times = 2,
    bands = 3,
    tileWidth = 3,
    tileHeight = 2,
    frameUnit = 3,
    transform: affine = [1, 0, 0, 0, 1, 0],
    epsg = null,
    pixelIsPoint = false,
  } = options;
  const bandTexts = options.bandTexts
    ?? Array.from({ length: bands }, (_, band) => `Band ${band}`);
  const timeKind = options.timeKind ?? "instant";
  const timeCoordinates = options.timeCoordinates ?? (timeKind === "instant"
    ? Array.from({ length: times }, (_, time) => 1700000001n + BigInt(time * 86400))
    : Array.from({ length: times }, (_, time) => {
      const start = 1700000001n + BigInt(time * 86400);
      return [start, start + 3600n];
    }).flat());
  const trailer = makeTrailer(bandTexts, timeKind, timeCoordinates);
  const counts = fixture.frames.map((frame) => frame.byteLength);
  const base = baseOffset(bands, counts.length);
  const offsets = [];
  let frameAt = base;
  for (const count of counts) {
    offsets.push(frameAt);
    frameAt += count;
  }

  const [a, b, c, d, e, f] = affine;
  const transform = [a, b, 0, c, d, e, 0, f, 0, 0, 0, 0, 0, 0, 0, 1];
  const model = epsg === null ? 0 : epsg === 4326 ? 2 : 1;
  const crsKey = model === 1 ? 3072 : 2048;
  const geokeys = [
    1, 1, 0, 3,
    1024, 0, 1, model,
    1025, 0, 1, pixelIsPoint ? 2 : 1,
    crsKey, 0, 1, epsg ?? 0,
  ];
  const entries = [
    [256, 4, integerBytes([width], 4)],
    [257, 4, integerBytes([height], 4)],
    [258, 3, integerBytes(Array(bands).fill(16), 2)],
    [277, 3, integerBytes([bands], 2)],
    [322, 3, integerBytes([tileWidth], 2)],
    [323, 3, integerBytes([tileHeight], 2)],
    [324, 16, integerBytes(offsets, 8)],
    [325, 4, integerBytes(counts, 4)],
    [339, 3, integerBytes(Array(bands).fill(1), 2)],
    [34264, 12, doubleBytes(transform)],
    [34735, 3, integerBytes(geokeys, 2)],
    [65000, 3, integerBytes([frameUnit], 2)],
    [65001, 4, integerBytes([times], 4)],
  ];
  const typeWidth = { 3: 2, 4: 4, 12: 8, 16: 8 };
  const file = new Uint8Array(frameAt + trailer.byteLength);
  const view = new DataView(file.buffer);
  file.set([0x52, 0x55, 0x4d, 0x49], 0);
  view.setUint16(4, 1, true);
  view.setBigUint64(8, 16n, true);
  view.setBigUint64(16, 13n, true);

  let externalAt = 16 + 276;
  entries.forEach(([tag, type, value], index) => {
    const at = 24 + index * 20;
    view.setUint16(at, tag, true);
    view.setUint16(at + 2, type, true);
    view.setBigUint64(at + 4, BigInt(value.byteLength / typeWidth[type]), true);
    if (value.byteLength <= 8) {
      file.set(value, at + 12);
    } else {
      view.setBigUint64(at + 12, BigInt(externalAt), true);
      file.set(value, externalAt);
      externalAt += value.byteLength + (value.byteLength & 1);
    }
  });
  if (externalAt !== base) throw new Error("fixture base offset mismatch");
  for (let index = 0; index < fixture.frames.length; index += 1) {
    file.set(fixture.frames[index], offsets[index]);
  }
  file.set(trailer, frameAt);
  return { ...fixture, file, bandTexts, timeKind, timeCoordinates, trailerOffset: frameAt };
}

export function expectedValues({ times, bands, window }) {
  const [y, x, height, width] = window;
  const values = [];
  for (const time of times) {
    for (const band of bands) {
      for (let yy = y; yy < y + height; yy += 1) {
        for (let xx = x; xx < x + width; xx += 1) {
          values.push(sampleValue(time, band, yy, xx));
        }
      }
    }
  }
  return values;
}
