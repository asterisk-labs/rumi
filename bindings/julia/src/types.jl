primitive type Float8E4M3FN <: AbstractFloat 8 end
primitive type Float8E5M2 <: AbstractFloat 8 end
primitive type Float8E8M0FNU <: AbstractFloat 8 end
primitive type Float8E4M3FNUZ <: AbstractFloat 8 end
primitive type Float8E5M2FNUZ <: AbstractFloat 8 end

@static if isdefined(Core, :BFloat16)
    const RumiBFloat16 = Core.BFloat16
else
    primitive type RumiBFloat16 <: AbstractFloat 16 end
end

struct _Header
    image_width::UInt32
    image_length::UInt32
    time_count::UInt32
    tile_width::UInt16
    tile_length::UInt16
    samples_per_pixel::UInt16
    bits_per_sample::UInt8
    sample_format::UInt8
    frame_unit::UInt8
    tiles_across::UInt32
    tiles_down::UInt32
    base_frame_offset::UInt64
    dtype::Cint
end

struct _Layout
    shape::NTuple{5,Int64}
    ndim::Cint
    stride::NTuple{5,Int64}
    native::Cint
end

struct _Metadata
    fields::_Header
    blob::Ptr{UInt8}
    blob_size::Csize_t
    has_source::Cint
    transform::NTuple{6,Cdouble}
    epsg::UInt32
    pixel_is_point::Cint
    band_texts::Ptr{Cstring}
    band_text_count::Csize_t
    time_type::UInt8
    time::Ptr{Int64}
    time_coords::Csize_t
end

struct _ReadItem
    source::Ptr{Cvoid}
    spec::Ptr{Cvoid}
    y_off::Cint
    x_off::Cint
end

struct Metadata
    header::Vector{UInt8}
    shape::Tuple
    time_count::Int
    dtype::String
    tile::Tuple{Int,Int}
    frame_layout::String
    index_order::Tuple
    frames::Int
    bands::Union{Nothing,Vector{String}}
    time::Union{Nothing,Vector}
    time_kind::Union{Nothing,Symbol}
    transform::Union{Nothing,NTuple{6,Float64}}
    crs::Union{Nothing,Int}
    pixel_is_point::Union{Nothing,Bool}
end

mutable struct Dataset
    source::Ptr{Cvoid}
    spec::Ptr{Cvoid}
    metadata::Metadata
end

info(file::Dataset) = file.metadata
isopen(file::Dataset) = file.source != C_NULL && file.spec != C_NULL

function show(io::IO, file::Dataset)
    state = isopen(file) ? "open" : "closed"
    print(io, "Rumi.Dataset(", state, ", shape=", file.metadata.shape,
          ", dtype=", repr(file.metadata.dtype), ")")
end

function close(file::Dataset)
    source, spec = file.source, file.spec
    file.source = C_NULL
    file.spec = C_NULL
    source == C_NULL || ccall(_sym(:rumi_source_free), Cvoid, (Ptr{Cvoid},), source)
    spec == C_NULL || ccall(_sym(:rumi_spec_destroy), Cvoid, (Ptr{Cvoid},), spec)
    return nothing
end

const _DTYPES = Dict{Int,Tuple{DataType,String}}(
    1 => (UInt8, "uint8"),
    2 => (Int8, "int8"),
    3 => (UInt16, "uint16"),
    4 => (Int16, "int16"),
    5 => (UInt32, "uint32"),
    6 => (Int32, "int32"),
    7 => (UInt64, "uint64"),
    8 => (Int64, "int64"),
    9 => (Float16, "float16"),
    10 => (Float32, "float32"),
    11 => (Float64, "float64"),
    14 => (Complex{Float16}, "complex32"),
    15 => (ComplexF32, "complex64"),
    16 => (ComplexF64, "complex128"),
    17 => (Float8E4M3FN, "float8_e4m3fn"),
    18 => (Float8E5M2, "float8_e5m2"),
    19 => (RumiBFloat16, "bfloat16"),
    24 => (Bool, "bool"),
    25 => (Float8E8M0FNU, "float8_e8m0fnu"),
    29 => (Float8E4M3FNUZ, "float8_e4m3fnuz"),
    30 => (Float8E5M2FNUZ, "float8_e5m2fnuz"),
)

function _dtype(code::Integer)
    haskey(_DTYPES, code) || throw(RumiError("librumi returned unknown dtype code $code"))
    return _DTYPES[code]
end
