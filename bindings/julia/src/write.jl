struct _WriteDesc
    image_width::UInt32
    image_length::UInt32
    time_count::UInt32
    tile_size::UInt16
    samples_per_pixel::UInt16
    dtype::Cint
    transform::Ptr{Cdouble}
    epsg::UInt32
    pixel_is_point::Cint
    frame_unit::UInt8
    band_texts::Ptr{Ptr{UInt8}}
    band_text_count::UInt64
    time_type::UInt8
    time::Ptr{Int64}
    time_coords::UInt64
end

function _payloads(table::FrameTable)
    payloads = table.compressed
    length(payloads) == length(table.data) || throw(ArgumentError(
        "compressed must have one entry per frame"))
    missing = findfirst(isnothing, payloads)
    missing === nothing || throw(ArgumentError("frame $missing has no compressed bytes"))
    return Vector{Vector{UInt8}}(payloads)
end

function _transform(transform)
    values = transform isa Union{Tuple,AbstractVector} ? collect(transform) : []
    length(values) in (6, 9) && all(v -> v isa Real && !(v isa Bool), values) ||
        throw(ArgumentError("a transform is six coefficients " *
                            "(x_res, row_rot, x_origin, col_rot, y_res, y_origin)"))
    length(values) == 9 && values[7:9] != [0, 0, 1] &&
        throw(ArgumentError("a nine-value affine transform must end in (0, 0, 1)"))
    return Cdouble[values[i] for i in 1:6]
end

function _epsg(crs)
    code = if crs isa Integer && !(crs isa Bool)
        crs
    elseif crs isa AbstractString
        m = match(r"^(?:[Ee][Pp][Ss][Gg]:)?([0-9]+)$", strip(crs))
        m === nothing ? nothing : tryparse(Int, m.captures[1])
    end
    code !== nothing && 1 <= code <= typemax(UInt32) ||
        throw(ArgumentError("crs must be an EPSG code such as 32631 or \"EPSG:32631\""))
    return UInt32(code)
end

# The core checks the count, emptiness, UTF-8 and uniqueness of the texts.
function _band_texts(bands)
    bands isa AbstractSet && throw(ArgumentError(
        "bands must be ordered; a set cannot preserve band order"))
    bands isa AbstractVector || throw(ArgumentError(
        "bands is a vector with one text per band; wrap a single text as " *
        "bands=[$(repr(bands))]"))
    texts = String[]
    for (band, text) in enumerate(bands)
        text isa AbstractString || throw(ArgumentError(
            "a band text is a string, got $(typeof(text)) for band $band"))
        '\0' in text && throw(ArgumentError(
            "band $band text contains a NUL character, which a C string cannot carry"))
        push!(texts, String(text))
    end
    return texts
end

"""
    write(path, table; bands, time, transform=nothing, crs=nothing,
          pixel_is_point=false) -> Vector{UInt8}

Write the compressed frames and return the external header. Every frame needs
compressed bytes. A failed write removes the incomplete file.

`bands` names each band. `time` has one `Date`, `DateTime` or ISO string per
step, or a `(start, end)` pair per step. Dates and datetimes are UTC.
`transform` takes six rasterio-order affine coefficients and requires an
EPSG `crs`.
"""
function write(path::AbstractString, table::FrameTable{T}; bands, time,
               transform=nothing, crs=nothing, pixel_is_point::Bool=false) where {T}
    payloads = _payloads(table)
    coefficients = transform === nothing ? Cdouble[] : _transform(transform)
    epsg = crs === nothing ? UInt32(0) : _epsg(crs)
    texts = _band_texts(bands)
    kind, seconds = _time_axis(time)

    out = Ref{Ptr{UInt8}}(C_NULL)
    out_size = Ref{Csize_t}(0)
    GC.@preserve payloads coefficients texts seconds begin
        text_ptrs = [pointer(text) for text in texts]
        frame_ptrs = [pointer(payload) for payload in payloads]
        sizes = Csize_t[length(payload) for payload in payloads]
        GC.@preserve text_ptrs frame_ptrs sizes begin
            desc = _WriteDesc(
                table.image_width, table.image_length, table.time_count,
                table.tile_size, table.bands, _dtype_code(T),
                isempty(coefficients) ? C_NULL : pointer(coefficients), epsg,
                pixel_is_point, table.frame_unit,
                pointer(text_ptrs), length(texts), kind, pointer(seconds), length(seconds),
            )
            _check(ccall(
                _sym(:rumi_write), Cint,
                (Cstring, Ref{_WriteDesc}, Ptr{Ptr{UInt8}}, Ptr{Csize_t}, Csize_t,
                 Ref{Ptr{UInt8}}, Ref{Csize_t}),
                expanduser(path), desc, frame_ptrs, sizes, length(frame_ptrs),
                out, out_size,
            ))
        end
    end
    try
        return copy(unsafe_wrap(Vector{UInt8}, out[], out_size[]))
    finally
        ccall(_sym(:rumi_free), Cvoid, (Ptr{Cvoid},), out[])
    end
end
