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
before calling [`write`](@ref). `row[i]` and `col[i]` give the one-based grid
cell of frame `i`. `band[i]` and `time[i]` give its band and time step when
frames are split by that axis, and `band` and `time` are `nothing` otherwise.
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
    pattern::String
    tiles_across::Int
    tiles_down::Int
    row::Vector{Int}
    col::Vector{Int}
    band::Union{Nothing,Vector{Int}}
    time::Union{Nothing,Vector{Int}}
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
    across, down, out = Ref{UInt32}(0), Ref{UInt32}(0), Ref{UInt64}(0)
    _check(ccall(
        _sym(:rumi_frame_count), Cint,
        (UInt8, UInt32, UInt32, UInt16, UInt16, UInt32,
         Ref{UInt32}, Ref{UInt32}, Ref{UInt64}),
        unit, width, len, tile, bands, times, across, down, out,
    ))
    return Int(across[]), Int(down[]), Int(out[])
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

function _tile_size(tile_size)
    tile_size isa Integer && !(tile_size isa Bool) ||
        throw(ArgumentError("tile_size must be an integer"))
    tile_size >= 0 || throw(ArgumentError("tile_size must not be negative, got $tile_size"))
    tile_size <= typemax(UInt16) ||
        throw(ArgumentError("tile_size must be at most 65535, got $tile_size"))
    return Int(tile_size)
end

_input_roles(compiled::_FramePattern) =
    [_axis_name(compiled.input[i]) for i in 1:Int(compiled.input_ndim)]

# Julia dimension d holds the pattern's axis n - d + 1.
function _extents(role, dims)
    extent(name) = (d = findfirst(==(name), role); d === nothing ? 1 : Int(dims[d]))
    bands, times, len, width = extent(:b), extent(:t), extent(:y), extent(:x)
    bands <= typemax(UInt16) ||
        throw(ArgumentError("band count must be in 1:65535, got $bands"))
    max(times, len, width) <= typemax(UInt32) ||
        throw(ArgumentError("time, height and width must each fit in 32 bits"))
    return bands, times, len, width
end

"""
    frames(x, pattern, tile_size=512) -> FrameTable
    frames(x, pattern; tile_size=512) -> FrameTable

Cut `x` into frames in file order. The pattern names axes in C order; Julia
holds them in reverse. For example, `"b (row h) (col w) -> row col b (h w)"`
takes an `(x, y, band)` array. Edge frames are clipped. The element type
sets the file's sample type.
"""
frames(x::AbstractArray, pattern::AbstractString, tile_size) =
    frames(x, pattern; tile_size)

function frames(x::AbstractArray, pattern::AbstractString; tile_size=512)
    Base.require_one_based_indexing(x)
    T = eltype(x)
    code = _dtype_code(T)
    tile = _tile_size(tile_size)
    compiled = _frame_pattern(pattern)
    roles = _input_roles(compiled)
    n = length(roles)
    ndims(x) == n || throw(ArgumentError(
        "the pattern names $n axes ($(join(roles, ' '))), so x needs $n dimensions, " *
        "got $(ndims(x))"))

    role = reverse(roles)
    bands, times, len, width = _extents(role, size(x))

    # Singleton band and time axes are omitted from the stored layout.
    unit = _frame_unit(compiled, bands, times)
    layout = _unit_name(unit, bands, times)
    held = Symbol.(split(layout))
    cut = [[name for name in (:b, :t) if name in held]; :y; :x]
    kept = [name for name in role if name in (:y, :x) || name in held]
    walked = _index_order(unit, bands, times)

    across, down, total = _frame_count(unit, width, len, tile, bands, times)
    data = Vector{Array{T,length(held)}}(undef, total)
    rows, cols = Vector{Int}(undef, total), Vector{Int}(undef, total)
    band = :b in walked ? Vector{Int}(undef, total) : nothing
    step = :t in walked ? Vector{Int}(undef, total) : nothing
    for k in 1:total
        at = _frame_locate(unit, width, len, tile, bands, times, k - 1)
        rows[k], cols[k] = Int(at.row) + 1, Int(at.col) + 1
        band === nothing || (band[k] = Int(at.band) + 1)
        step === nothing || (step[k] = Int(at.time) + 1)
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
                      layout, dtype, String(strip(pattern)), across, down,
                      rows, cols, band, step)
end

"""
    frame_start(shape, pattern, tile_size=512) -> Int
    frame_start(shape, pattern; tile_size=512) -> Int

Return the byte offset of the first frame that [`frames`](@ref) and
[`write`](@ref) would give an array of size `shape`, without cutting it. The
offset is also the size of everything the file stores before its first frame.
`shape` is in `size(x)` order, the reverse of the pattern's input axes.
"""
frame_start(shape, pattern::AbstractString, tile_size) =
    frame_start(shape, pattern; tile_size)

function frame_start(shape, pattern::AbstractString; tile_size=512)
    shape isa Union{Tuple,AbstractVector} &&
        all(value -> value isa Integer && !(value isa Bool), shape) ||
        throw(ArgumentError("shape must be a tuple or vector of integers"))
    all(>=(0), shape) || throw(ArgumentError("shape must not hold a negative size"))
    tile = _tile_size(tile_size)
    compiled = _frame_pattern(pattern)
    roles = _input_roles(compiled)
    n = length(roles)
    length(shape) == n || throw(ArgumentError(
        "the pattern names $n axes ($(join(roles, ' '))), so shape needs $n sizes, " *
        "got $(length(shape))"))
    bands, times, len, width = _extents(reverse(roles), shape)
    unit = _frame_unit(compiled, bands, times)
    count = _frame_count(unit, width, len, tile, bands, times)[3]
    out = Ref{UInt64}(0)
    _check(ccall(
        _sym(:rumi_frame_start), Cint,
        (UInt32, UInt64, Ref{UInt64}), bands, count, out,
    ))
    return Int(out[])
end
