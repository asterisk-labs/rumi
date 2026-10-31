const rows = [
  [1, "uint8", 1, 8, 1, 1, Uint8Array],
  [2, "int8", 2, 8, 1, 1, Int8Array],
  [3, "uint16", 1, 16, 2, 2, Uint16Array],
  [4, "int16", 2, 16, 2, 2, Int16Array],
  [5, "uint32", 1, 32, 4, 4, Uint32Array],
  [6, "int32", 2, 32, 4, 4, Int32Array],
  [7, "uint64", 1, 64, 8, 8, BigUint64Array],
  [8, "int64", 2, 64, 8, 8, BigInt64Array],
  [9, "float16", 3, 16, 2, 2, Uint16Array],
  [10, "float32", 3, 32, 4, 4, Float32Array],
  [11, "float64", 3, 64, 8, 8, Float64Array],
  [14, "complex32", 6, 32, 4, 2, Uint16Array],
  [15, "complex64", 6, 64, 8, 4, Float32Array],
  [16, "complex128", 6, 128, 16, 8, Float64Array],
  [17, "float8_e4m3fn", 100, 8, 1, 1, Uint8Array],
  [18, "float8_e5m2", 101, 8, 1, 1, Uint8Array],
  [19, "bfloat16", 102, 16, 2, 2, Uint16Array],
  [24, "bool", 1, 1, 1, 1, Uint8Array],
  [25, "float8_e8m0fnu", 103, 8, 1, 1, Uint8Array],
  [29, "float8_e4m3fnuz", 107, 8, 1, 1, Uint8Array],
  [30, "float8_e5m2fnuz", 108, 8, 1, 1, Uint8Array],
];

const table = new Map();
const littleEndian = new Uint8Array(new Uint16Array([1]).buffer)[0] === 1;

for (const [code, name, sampleFormat, bits, bytes, componentBytes, ArrayType] of rows) {
  const dtype = Object.freeze({
    code,
    name,
    sampleFormat,
    bits,
    bytes,
    componentBytes,
    ArrayType,
  });
  table.set(`${sampleFormat}:${bits}`, dtype);
}

export function dtypeFromSample(sampleFormat, bits) {
  return table.get(`${sampleFormat}:${bits}`);
}

export function storageView(bytes, dtype) {
  if (!littleEndian) throw new Error("Rumi requires a little-endian host");
  return new dtype.ArrayType(bytes.buffer, bytes.byteOffset, bytes.byteLength / dtype.componentBytes);
}
