# pfor Codec Specification

Lossless numeric codec, CTID `0x72D70D`. The stream is cut into blocks of 256
values, each packed at a selected bit width. Values that do not fit carry their
high bits in a per-block exception list.

## Inputs

One serial stream, the packed blocks back to back.

## Codec header

Little endian, 9 bytes:

- bytes 0-7: element count, as `uint64`;
- byte 8: element width in bytes, one of 1, 2, 4, 8.

Neither is recoverable from a serial stream, so both travel. A count of zero is
valid and the serial stream is then empty; any other count with an empty stream
is corrupt.

The count drives the output allocation and arrives on the wire, so a reader
bounds it before use: every block spends at least its two header bytes and
carries 256 values, so a stream of `S` bytes cannot describe more than
`S * 128` elements.

## Block layout

The stream contains `ceil(nbElts / 256)` blocks. A short final block is zero
padded to 256 on encode; the padding is packed like any other value and
discarded on decode.

Each block, in order:

| field | size | meaning |
|---|---|---|
| `b` | `uint8` | bits per value in the body, `0 <= b <= 8*eltWidth` |
| `nexc` | `uint8` | exception count, `0 <= nexc <= 255`. A block needing more is re-encoded at a wider `b`, which always exists |
| `eb`, `mode` | `uint8`, only when `nexc > 0` | low 7 bits are `eb`, the bits each exception carries above `b`; the top bit is 0 for a bitmap and 1 for a list |
| positions | 32 bytes, or `nexc` bytes | bitmap or list, only when `nexc > 0` |
| exception bits | `ceil(nexc * eb / 8)` bytes | only when `nexc > 0` |
| body | `32 * b` bytes | the packed values |

`b + eb` must not exceed `8 * eltWidth`, and `eb` must be nonzero when
`nexc > 0`.

In bitmap mode the 32 bytes hold one bit per position, LSB first within each
byte, set where the value is an exception; the number of set bits must equal
`nexc`. In list mode the `nexc` bytes are positions in strictly ascending order.
Every byte is a valid position in a 256-value block.

Exception bits are LSB first, `eb` bits per exception, in position order.

## Body layout

The body packs 256 values into `2 * b` groups of 16 bytes. With `W`-byte elements a
group holds `L = 16 / W` lanes; value `i` belongs to lane `i mod L` and takes
slot `i div L` inside it. Each lane packs its own `S = 256 / L` values into a
plain bit stream of `S * b` bits, LSB first, laid out as `2 * b` little-endian
`W`-byte words. Word `j` of lane `l` is at byte offset `j * 16 + l * W` of the
body.

With a 256-value block, `S` is a multiple of `8W` for every supported element
width. Each lane stream therefore ends on a `W`-byte word boundary for every
value of `b`.

The interleave exists so that a decoder recovers one slot across all lanes with
a single shift and mask and no lane crossing. The group is 16 bytes whatever
vector unit is present: the layout is part of the format, and a frame written on
one machine is read on another.

## Decoding

For each block, unpack the body, then for each exception in position order read
`eb` bits and set

    value = body_value | (exception_bits << b)

An exception keeps its low `b` bits in the body like every other value, so no
value is reserved as a sentinel and the body is uniform.

The stream must end exactly at the last block. Trailing bytes are corrupt.

## Output

One numeric stream of `nbElts` elements at the header's element width.

## Notes for an encoder

The choice of `b` per block is not part of the format. A decoder accepts any
`b` a block declares, so an encoder may change its selection strategy without a
new CTID. This encoder tests every width from the block maximum to zero and adds
a four-bit cost per exception to the encoded size.

## Credit

The block design follows
[TurboPFor](https://github.com/powturbo/TurboPFor-Integer-Compression/graphs/contributors)
by powturbo and contributors. Independent implementation, not bitstream
compatible.
