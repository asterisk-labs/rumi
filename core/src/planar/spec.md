## planar Decoder Specification
### Inputs
A single numeric stream of 8, 16, 32 or 64-bit integers holding the planar residual plane in row major order.

### Codec Header
A single uint32, little endian, the row width in samples. The number of rows is the element count divided by the width.

### Decoding
The predictor is W + N - NW, where W is the left reconstructed sample, N the sample above and NW the sample above left. Edge neighbors are zero, so row zero reduces to the horizontal predictor and column zero to the vertical one. Each row is reconstructed by adding N - NW from the row above into the residual, then a prefix sum over the row resolves the W chain, all in native width modular arithmetic.

### Outputs
A single numeric stream of the same element width and the same length as the input.
