## delta_n Decoder Specification
### Inputs
A single numeric stream of 8, 16, 32 or 64-bit integers holding the vertical residual plane in row major order.

### Codec Header
A single uint32, little endian, the row width in samples. The number of rows is the element count divided by the width.

### Decoding
The first row is copied through as absolute values. Every later row is reconstructed by adding the reconstructed row above to the residual, sample by sample, using native width modular addition. Columns are independent, there is no horizontal carry.

### Outputs
A single numeric stream of the same element width and the same length as the input.
