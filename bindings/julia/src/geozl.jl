"""
    Rumi.GeoZL

Compress frames with the GeoZL linked into Rumi. Build a graph with
[`GeoZL.graph`](@ref), then pass the bytes from [`GeoZL.compress`](@ref)
to `Rumi.write`. It covers `graph` and `compress` only; `decompress`,
`profile` and `coeffs` live in GeoZL.jl. Argument errors read as in the GeoZL
packages. `librumi` makes every check it can see, and the binding only those
that need a Julia value, as GeoZL.jl makes them.
"""
module GeoZL

import ..Rumi: _check, _dtype, _dtype_code, _sym

# Other supported types compress losslessly as raw samples.
const _Sample = Union{UInt8,UInt16,UInt32,UInt64,Int8,Int16,Int32,Int64,
                      Float16,Float32,Float64}

# The Rumi name of a sample type, such as "uint16".
_name(::Type{T}) where {T} = _dtype(_dtype_code(T))[2]

# One sample of T for `nodata`. The core refuses a sentinel on a dtype outside
# the eleven, so such a dtype gets a placeholder of zero bits.
function _convert_nodata(::Type{T}, nodata) where {T}
    nodata isa Real && !(nodata isa Bool) ||
        throw(ArgumentError("`nodata` must be a number, got $(repr(nodata))"))
    T <: _Sample || return reinterpret(T, ntuple(_ -> 0x00, sizeof(T)))
    name = _name(T)
    if nodata isa AbstractFloat && isnan(nodata) && !(T <: AbstractFloat)
        throw(ArgumentError("a $name raster cannot use NaN as nodata"))
    end
    value = try
        convert(T, nodata)
    catch err
        err isa Union{InexactError,DivideError,MethodError} || rethrow()
        throw(ArgumentError("nodata $nodata does not fit $name"))
    end
    isinf(value) && !isinf(nodata) && throw(ArgumentError("nodata $nodata does not fit $name"))
    return value
end

# Replace missing values with the sentinel or NaN before passing to C.
function _samples(x::AbstractArray, nodata)
    T = nonmissingtype(eltype(x))
    # Union{} otherwise passes the subtype check below.
    T === Union{} && throw(ArgumentError("`x` holds no samples, only missing values"))
    _dtype_code(T)
    eltype(x) === T && return x isa Array ? x : Array{T}(x)
    any(ismissing, x) || return Array{T}(x)
    if nodata === nothing && !(T <: Union{Float16,Float32,Float64})
        throw(ArgumentError("`x` has missing values; a $(_name(T)) raster needs " *
                            "`nodata` to mark them"))
    end
    fill = nodata === nothing ? T(NaN) : _convert_nodata(T, nodata)
    return T[ismissing(v) ? fill : v for v in x]
end

function _method(method::AbstractString)
    isempty(method) &&
        throw(ArgumentError("`method` must be a recipe such as \"planar>zigzag>pfor\""))
    '\0' in method && throw(ArgumentError("`method` must not contain a NUL character"))
    return String(method)
end

const _NUMBER = r"^[+-]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?$"

# A positive finite float as Python's repr spells a float64 and NumPy's str a
# float32 or float16, as GeoZL.jl does, so every binding writes one recipe.
# The digits are the shortest that read back, positional from 1e-4 up to 1e16
# (1e6 for float32, 1e3 for float16) and scientific outside, with a signed
# exponent of at least two digits.
function _python_float(x::Union{Float16,Float32,Float64})
    text = replace(string(x), r"^Float16\((.*)\)$" => s"\1")
    m = match(r"^(\d+)(?:\.(\d+))?(?:[ef]([+-]?\d+))?$", text)
    digits = m[1] * something(m[2], "")
    point = length(m[1]) + (m[3] === nothing ? 0 : parse(Int, m[3]))
    lead = findfirst(!=('0'), digits)
    digits = rstrip(digits[lead:end], '0')
    point -= lead - 1
    limit = x isa Float16 ? 1e3 : x isa Float32 ? 1e6 : 1e16
    if 1e-4 <= Float64(x) < limit
        point <= 0 && return "0." * "0"^-point * digits
        point >= length(digits) && return digits * "0"^(point - length(digits)) * ".0"
        return digits[1:point] * "." * digits[point+1:end]
    end
    mantissa = length(digits) == 1 ? digits : digits[1:1] * "." * digits[2:end]
    return mantissa * "e" * (point > 0 ? "+" : "-") * lpad(abs(point - 1), 2, '0')
end

# The core normalizes and checks the text. It reads a bare number in text as
# LINEAR and empty text as lossless, which GeoZL.jl refuses, so this does too.
function _error_text(error)
    error === nothing && return nothing
    error isa Bool && throw(ArgumentError("`error` must not be a Bool"))
    if error isa Real
        error isa Integer && return string(error)
        value = error isa Union{Float16,Float32,Float64} ? error : Float64(error)
        # The core refuses a sign, an infinity or NaN, and reads zero as lossless.
        isfinite(error) || return string(value)
        isfinite(value) ||
            throw(ArgumentError("`error` cannot be represented as a finite number"))
        iszero(value) && !iszero(error) &&
            throw(ArgumentError("`error` $error is too small for a Float64"))
        iszero(value) && return "0.0"
        return (signbit(value) ? "-" : "") * _python_float(abs(value))
    end
    error isa AbstractString ||
        throw(ArgumentError("`error` must be a number, percentage or recipe, got $(repr(error))"))
    '\0' in error && throw(ArgumentError("`error` must not contain a NUL character"))
    (isempty(error) || occursin(_NUMBER, error)) &&
        throw(ArgumentError("error must be a number, a percentage such as \"10%\", " *
                            "or a LINEAR, LOG or SQRT recipe, got $(repr(error))"))
    return String(error)
end

function _u32(value, name)
    value isa Integer && !(value isa Bool) && 1 <= value <= typemax(UInt32) ||
        throw(ArgumentError("`$name` must be an integer from 1 to 4294967295, got $(repr(value))"))
    return UInt32(value)
end

# Julia's first dimension is the row, unlike Python's last dimension.
function _geometry(x::AbstractArray, width, planes)
    if width === nothing
        ndims(x) >= 2 || throw(ArgumentError("give `width` for a vector"))
        width = size(x, 1)
    end
    planes === nothing && (planes = ndims(x) >= 3 ? size(x, ndims(x)) : 1)
    return _u32(width, "width"), _u32(planes, "planes")
end

"""
    Graph{T}

A reusable graph from [`graph`](@ref) for samples of type `T`, which
`eltype(g)` returns. `dtype` holds the Rumi name of `T`, such as `"uint16"`,
and `itemsize` its bytes per sample. Tasks may share it, and their compress
calls take turns.
"""
mutable struct Graph{T}
    ptr::Ptr{Cvoid}
    method::String
    width::Int
    planes::Int
    dtype::String
    itemsize::Int
    error::Union{Nothing,String}
    nodata::Any

    function Graph{T}(ptr, method, width, planes, error, nodata) where {T}
        g = new{T}(ptr, method, width, planes, _name(T), sizeof(T), error, nodata)
        return finalizer(g) do g
            g.ptr == C_NULL || ccall(_sym(:rumi_geozl_graph_close), Cvoid, (Ptr{Cvoid},), g.ptr)
            g.ptr = C_NULL
        end
    end
end

Base.eltype(::Type{Graph{T}}) where {T} = T

function Base.show(io::IO, g::Graph)
    plane_word = g.planes == 1 ? "plane" : "planes"
    print(io, "Rumi.GeoZL.Graph(", g.method, ", ", g.dtype, ", row width ", g.width, ", ",
          g.planes, " ", plane_word, ", ", something(g.error, "lossless"), ")")
end

"""
    graph(x, method; width=nothing, planes=nothing, error=nothing, nodata=nothing)

Build a graph for `method` from raster `x`. The graph fixes the element type,
geometry and, when lossy, the domain accepted for later tiles.

`width` defaults to `size(x, 1)`; `planes` defaults to the last dimension for
arrays with at least three dimensions. Each is an integer from 1 to
4294967295. Row predictors restart at each plane.

`error` accepts a number (LINEAR), a percentage (LOG), or a full recipe.
`nothing` and zero mean lossless. For lossy compression, build the graph from
data covering the intended range.

`nodata` sets a sentinel for `missing` values. NaNs in a float raster are
detected automatically when no sentinel is given.
"""
function graph(x::AbstractArray, method::AbstractString; width=nothing, planes=nothing,
               error=nothing, nodata=nothing)
    method = _method(method)
    text = _error_text(error)
    a = _samples(x, nodata)
    T = eltype(a)
    sample = nodata === nothing ? nothing : Ref(_convert_nodata(T, nodata))
    w, p = _geometry(x, width, planes)
    out = Ref{Ptr{Cvoid}}(C_NULL)
    GC.@preserve a sample begin
        _check(ccall(_sym(:rumi_geozl_graph_open), Cint,
            (Cstring, Cint, Ptr{Cvoid}, Csize_t, UInt32, UInt32, Ptr{UInt8}, Ptr{Cvoid},
             Ref{Ptr{Cvoid}}),
            method, _dtype_code(T), a, sizeof(a), w, p,
            text === nothing ? C_NULL : text, sample === nothing ? C_NULL : sample, out))
    end
    g = Graph{T}(out[], method, Int(w), Int(p), nothing, nodata)
    # The core normalizes the short syntax, so the graph reports it as GeoZL does.
    recipe = ccall(_sym(:rumi_geozl_graph_error), Cstring, (Ptr{Cvoid},), g.ptr)
    recipe == C_NULL || (g.error = unsafe_string(recipe))
    return g
end

"""
    compress(x, graph::Graph) -> Vector{UInt8}
    compress(x; graph::Graph) -> Vector{UInt8}

Compress `x` with `graph` and return the frame. The element type must match.
The shape is not checked, but a row predictor refuses a tile longer than one
row that is not whole rows of the graph's width. Lossy tiles outside the
graph's domain are rejected.
"""
function compress(x::AbstractArray, graph::Graph)
    graph.ptr == C_NULL && throw(ArgumentError("this graph is closed"))
    a = _samples(x, graph.nodata)
    out = Ref{Ptr{UInt8}}(C_NULL)
    out_size = Ref{Csize_t}(0)
    GC.@preserve graph a begin
        _check(ccall(_sym(:rumi_geozl_compress), Cint,
            (Ptr{Cvoid}, Cint, Ptr{Cvoid}, Csize_t, Ref{Ptr{UInt8}}, Ref{Csize_t}),
            graph.ptr, _dtype_code(eltype(a)), a, sizeof(a), out, out_size))
    end
    try
        return copy(unsafe_wrap(Vector{UInt8}, out[], out_size[]))
    finally
        ccall(_sym(:rumi_free), Cvoid, (Ptr{Cvoid},), out[])
    end
end

compress(x::AbstractArray; graph) = compress(x, graph)

end
