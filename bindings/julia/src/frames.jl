struct _FramePattern
    input::NTuple{4,UInt8}
    input_ndim::Cint
    frame::NTuple{4,UInt8}
    frame_ndim::Cint
    index::NTuple{2,UInt8}
    index_ndim::Cint
end

struct _FrameAt
    row::UInt32
    col::UInt32
    band::UInt32
    time::UInt32
    h::UInt32
    w::UInt32
    dims::NTuple{4,Int64}
    perm::NTuple{4,UInt8}
    ndim::Cint
end

"""
    FrameTable

Frames in file order. `data[i]` holds the samples; fill `compressed[i]`
before calling [`write`](@ref).
"""
mutable struct FrameTable{T,N}
    data::Vector{Array{T,N}}
    compressed::Vector{Union{Nothing,Vector{UInt8}}}
    image_width::Int
    image_length::Int
    tile_size::Int
    bands::Int
    time_count::Int
    frame_unit::UInt8
    layout::String
    dtype::String
end

Base.length(table::FrameTable) = length(table.data)

function show(io::IO, table::FrameTable)
    done = count(!isnothing, table.compressed)
    print(io, "Rumi.FrameTable(", length(table), " frames, layout=",
          repr(table.layout), ", dtype=", repr(table.dtype),
          ", compressed=", done, ")")
end

const _CODES = Dict{DataType,Cint}(T => Cint(code) for (code, (T, _)) in _DTYPES)

function _dtype_code(T)
    code = get(_CODES, T, nothing)
    code === nothing && throw(ArgumentError("Rumi does not store $T samples"))
    return code
end

_axis_name(axis::Integer) =
    Symbol(unsafe_string(ccall(_sym(:rumi_axis_name), Cstring, (UInt8,), axis)))

function _frame_pattern(pattern::AbstractString)
    out = Ref{_FramePattern}()
    _check(ccall(
        _sym(:rumi_compile_frame_pattern), Cint,
        (Cstring, Ref{_FramePattern}), pattern, out,
    ))
    return out[]
end

function _frame_unit(pattern::_FramePattern, bands, times)
    out = Ref{UInt8}(0)
    _check(ccall(
        _sym(:rumi_frame_unit), Cint,
        (Ref{_FramePattern}, UInt16, UInt32, Ref{UInt8}),
        pattern, bands, times, out,
    ))
    return out[]
end

function _frame_count(unit, width, len, tile, bands, times)
    out = Ref{UInt64}(0)
    _check(ccall(
        _sym(:rumi_frame_count), Cint,
        (UInt8, UInt32, UInt32, UInt16, UInt16, UInt32,
         Ptr{UInt32}, Ptr{UInt32}, Ref{UInt64}),
        unit, width, len, tile, bands, times, C_NULL, C_NULL, out,
    ))
    return Int(out[])
end

function _frame_locate(unit, width, len, tile, bands, times, index)
    out = Ref{_FrameAt}()
    _check(ccall(
        _sym(:rumi_frame_locate), Cint,
        (UInt8, UInt32, UInt32, UInt16, UInt16, UInt32, UInt64, Ref{_FrameAt}),
        unit, width, len, tile, bands, times, index, out,
    ))
    return out[]
end

"""
    frames(x, pattern; tile_size=512) -> FrameTable

Cut `x` into frames in file order. The pattern names axes in C order; Julia
holds them in reverse. For example, `"b (row h) (col w) -> row col b (h w)"`
takes an `(x, y, band)` array. Edge frames are clipped. The element type
sets the file's sample type.
"""
function frames(x::AbstractArray, pattern::AbstractString; tile_size::Integer=512)
    Base.require_one_based_indexing(x)
    T = eltype(x)
    code = _dtype_code(T)
    tile_size isa Bool && throw(ArgumentError("tile_size must be an integer"))
    tile_size >= 0 || throw(ArgumentError("tile_size must not be negative, got $tile_size"))
    tile_size <= typemax(UInt16) ||
        throw(ArgumentError("tile_size must be at most 65535, got $tile_size"))
    tile = Int(tile_size)
    compiled = _frame_pattern(pattern)
    roles = [_axis_name(compiled.input[i]) for i in 1:Int(compiled.input_ndim)]
    n = length(roles)
    ndims(x) == n || throw(ArgumentError(
        "the pattern names $n axes ($(join(roles, ' '))), so x needs $n dimensions, " *
        "got $(ndims(x))"))

    # Julia dimension d holds the pattern's axis n - d + 1.
    role = reverse(roles)
    extent(name) = (d = findfirst(==(name), role); d === nothing ? 1 : size(x, d))
    bands, times, len, width = extent(:b), extent(:t), extent(:y), extent(:x)
    bands <= typemax(UInt16) ||
        throw(ArgumentError("band count must be in 1:65535, got $bands"))
    max(times, len, width) <= typemax(UInt32) ||
        throw(ArgumentError("time, height and width must each fit in 32 bits"))

    # Singleton band and time axes are omitted from the stored layout.
    unit = _frame_unit(compiled, bands, times)
    layout = _unit_name(unit, bands, times)
    held = Symbol.(split(layout))
    cut = [[name for name in (:b, :t) if name in held]; :y; :x]
    kept = [name for name in role if name in (:y, :x) || name in held]

    total = _frame_count(unit, width, len, tile, bands, times)
    data = Vector{Array{T,length(held)}}(undef, total)
    for k in 1:total
        at = _frame_locate(unit, width, len, tile, bands, times, k - 1)
        ys = Int(at.row) * tile .+ (1:Int(at.h))
        xs = Int(at.col) * tile .+ (1:Int(at.w))
        index = map(role) do name
            if name === :y
                ys
            elseif name === :x
                xs
            elseif name in held
                Colon()
            elseif name === :b
                Int(at.band) + 1
            else
                Int(at.time) + 1
            end
        end
        source = view(x, index...)
        # perm orders the cut (held b, held t, y, x) as the frame's C axes,
        # which a Julia frame lists in reverse.
        order = reverse([cut[at.perm[i] + 1] for i in 1:Int(at.ndim)])
        perm = [findfirst(==(name), kept) for name in order]
        frame = Array{T}(undef, ntuple(i -> size(source, perm[i]), length(perm)))
        permutedims!(frame, source, perm)
        if T === Bool
            GC.@preserve frame _check(ccall(
                _sym(:rumi_check_samples), Cint,
                (Ptr{Cvoid}, Csize_t, Cint), frame, sizeof(frame), code,
            ))
        end
        data[k] = frame
    end

    _, dtype = _dtype(code)
    return FrameTable(data, Vector{Union{Nothing,Vector{UInt8}}}(nothing, total),
                      Int(width), Int(len), tile, Int(bands), Int(times), unit,
                      layout, dtype)
end
