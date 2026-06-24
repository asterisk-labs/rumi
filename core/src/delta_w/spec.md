## delta_w Decoder Specification
### Inputs
A single numeric stream of 8, 16, 32 or 64-bit integers holding the horizontal residual plane in row major order.

### Codec Header
A single uint32, little endian, the row width in samples. The number of rows is the element count divided by the width.

### Decoding
Each row is reconstructed on its own. The first sample of a row is its absolute value. Every later sample is the previous reconstructed sample plus the residual, using native width modular addition. The carry reseeds at every row edge, so rows do not affect each other.

Take a row of width 4 with residuals {5, 1, 1, 2}. The reconstructed row is {5, 6, 7, 9}.

### Outputs
A single numeric stream of the same element width and the same length as the input.
