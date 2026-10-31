export type RumiSource =
  | ArrayBuffer
  | SharedArrayBuffer
  | ArrayBufferView
  | Blob
  | string
  | URL
  | {
      read(offset: bigint, length: number): Promise<Uint8Array>;
      size(): bigint | Promise<bigint>;
    };

export interface GeoZLDecoder {
  decompress(
    frame: Uint8Array,
    options: { verify: boolean; maxOutputSize: number },
  ): ArrayBufferView | ArrayBuffer | SharedArrayBuffer | Promise<ArrayBufferView | ArrayBuffer | SharedArrayBuffer>;
}

export type Decoder = GeoZLDecoder | ((
  frame: Uint8Array,
  options: { verify: boolean; maxOutputSize: number },
) => ArrayBufferView | ArrayBuffer | SharedArrayBuffer | Promise<ArrayBufferView | ArrayBuffer | SharedArrayBuffer>);

export interface DType {
  readonly code: number;
  readonly name: string;
  readonly sampleFormat: number;
  readonly bits: number;
  readonly bytes: number;
  readonly componentBytes: number;
  readonly ArrayType: TypedArrayConstructor;
}

export type TypedArray =
  | Uint8Array | Int8Array | Uint16Array | Int16Array
  | Uint32Array | Int32Array | BigUint64Array | BigInt64Array
  | Float32Array | Float64Array;

export interface TypedArrayConstructor {
  new(buffer: ArrayBufferLike, byteOffset?: number, length?: number): TypedArray;
}

export interface ParsedHeader {
  readonly width: number;
  readonly height: number;
  readonly timeCount: number;
  readonly tileWidth: number;
  readonly tileHeight: number;
  readonly bands: number;
  readonly dtype: DType;
  readonly frameUnit: number;
  readonly frameAxes: readonly string[];
  readonly frameLayout: string;
  readonly timeFirst: boolean;
  readonly tilesAcross: number;
  readonly tilesDown: number;
  readonly frameCount: number;
  readonly maxDecodedFrame: number;
  readonly baseFrameOffset: bigint;
  readonly countMin: number;
  readonly countBits: number;
  frameOffset(index: number): bigint;
  frameByteCount(index: number): number;
  frameIndex(spatial: number, band: number, time: number): number;
}

export interface OpenOptions {
  decoder?: Decoder;
  geozl?: {
    wasmUrl?: string | URL;
    locateFile?: (path: string, prefix?: string) => string | URL;
    [option: string]: unknown;
  };
  maxFrameBytes?: number;
  maxMetadataBytes?: number;
  maxOutputBytes?: number;
  maxTimeCoordinates?: number;
  fetch?: typeof fetch;
  signal?: AbortSignal;
}

export interface RumiGeoreferencing {
  readonly transform: readonly [number, number, number, number, number, number] | null;
  readonly epsg: number | null;
  readonly crs: string | null;
  readonly pixelIsPoint: boolean;
}

export type RumiTime = bigint | readonly [start: bigint, end: bigint];

export interface RumiMetadata extends RumiGeoreferencing {
  readonly bands: readonly string[] | null;
  readonly time: readonly RumiTime[] | null;
  readonly timeKind: "instant" | "interval" | null;
}

export interface ReadOptions {
  time?: IndexArray;
  bands?: IndexArray;
  window?: readonly [y: number, x: number, height: number, width: number];
  verify?: boolean;
}

export type IndexArray = readonly number[]
  | Uint8Array | Uint16Array | Uint32Array
  | Int8Array | Int16Array | Int32Array
  | Float32Array | Float64Array;

export interface RumiInfo extends RumiMetadata {
  readonly shape: readonly number[];
  readonly axes: readonly string[];
  readonly dtype: DType;
  readonly tileShape: readonly [number, number];
  readonly gridShape: readonly [number, number];
  readonly frameUnit: number;
  readonly frameLayout: string;
  readonly frameCount: number;
}

export interface FrameRange {
  readonly frameIndex: number;
  readonly offset: bigint;
  readonly length: number;
}

export interface ReadResult {
  readonly data: TypedArray;
  readonly bytes: Uint8Array;
  readonly shape: readonly number[];
  readonly axes: readonly string[];
  readonly dtype: DType;
}

export declare class RumiDataset {
  private constructor();
  readonly header: ParsedHeader;
  info(): RumiInfo;
  planRanges(options?: ReadOptions): FrameRange[];
  read(options?: ReadOptions): Promise<ReadResult>;
}

export declare function openRumi(source: RumiSource, options?: OpenOptions): Promise<RumiDataset>;
