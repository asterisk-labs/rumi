"""
    Rumi.GeoZL

Compress frames with the GeoZL linked into Rumi. Build a graph with
[`GeoZL.graph`](@ref), then pass the bytes from [`GeoZL.compress`](@ref)
to `Rumi.write`.
"""
module GeoZL

import ..Rumi: _check, _dtype_code, _sym

# Other supported types compress losslessly as raw samples.
const _Sample = Union{UInt8,UInt16,UInt32,UInt64,Int8,Int16,Int32,Int64,
                      Float16,Float32,Float64}

function _convert_nodata(::Type{T}, nodata) where {T}
    nodata isa Real || throw(ArgumentError("`nodata` must be a number"))
    if nodata isa AbstractFloat && isnan(nodata) && !(T <: AbstractFloat)
        throw(ArgumentError("an integer raster cannot use NaN as nodata"))
    end
    value = try
        convert(T, nodata)
    catch err
        err isa InexactError || rethrow()
        throw(ArgumentError("nodata does not fit the element type"))
    end
    isinf(value) && !isinf(nodata) && throw(ArgumentError("nodata does not fit the element type"))
    return value
end

# The core rejects sentinels on other types; their sample is unused.
_nodata_sample(::Type{T}, nodata) where {T} =
    T <: _Sample ? Ref(_convert_nodata(T, nodata)) : Ref(ntuple(_ -> 0x00, 8))

_fill_value(::Type{T}, nodata) where {T} =
    nodata === nothing ? T(NaN) : _convert_nodata(T, nodata)

# Replace missing values with the sentinel or NaN before passing to C.
function _samples(x::AbstractArray, nodata)
    T = nonmissingtype(eltype(x))
    # Union{} otherwise passes the subtype check below.
    T === Union{} && throw(ArgumentError("`x` holds no samples, only missing values"))
    _dtype_code(T)
    eltype(x) === T && return x isa Array ? x : Array{T}(x)
    any(ismissing, x) || return Array{T}(x)
    T === Bool && throw(ArgumentError("`x` has missing values, which a Bool raster cannot hold"))
    if nodata === nothing && !(T <: Union{Float16,Float32,Float64})
        throw(ArgumentError("`x` has missing values; an integer raster needs `nodata` to mark them"))
    end
    fill = _fill_value(T, nodata)
    return T[ismissing(v) ? fill : v for v in x]
end

function _error_text(error)
    error === nothing && return nothing
    error isa Bool && throw(ArgumentError("`error` must not be a Bool"))
    if error isa Real
        value = error isa Integer || error isa AbstractFloat ? error : Float64(error)
        return string(value)
    end
    error isa AbstractString ||
        throw(ArgumentError("`error` must be a number, percentage or recipe"))
    '\0' in error &&
        throw(ArgumentError("`error` contains a NUL character, which a C string cannot carry"))
    # A bare number is LINEAR only as a number, as in GeoZL.jl.
    occursin(':', error) || endswith(error, "%") ||
        throw(ArgumentError("a text `error` is a percentage such as \"1%\" or a LINEAR, " *
                            "LOG or SQRT recipe"))
    return String(error)
end

# Julia's first dimension is the row, unlike Python's last dimension.
function _geometry(x::AbstractArray, width, planes)
    if width === nothing
        ndims(x) >= 2 || throw(ArgumentError("give `width` for a vector"))
        width = size(x, 1)
    end
    planes === nothing && (planes = ndims(x) >= 3 ? size(x, ndims(x)) : 1)
    fits(n) = n isa Integer && !(n isa Bool) && 0 <= n <= typemax(UInt32)
    fits(width) && fits(planes) ||
        throw(ArgumentError("`width` and `planes` must be whole numbers"))
    return UInt32(width), UInt32(planes)
end

"""
    Graph

A reusable graph from [`graph`](@ref). Its native context is not safe for
concurrent calls.
"""
mutable struct Graph
    ptr::Ptr{Cvoid}
    method::String
    width::Int
    planes::Int
    eltype::DataType
    error::Union{Nothing,String}
    nodata::Any

    function Graph(ptr, method, width, planes, T, error, nodata)
        g = new(ptr, method, width, planes, T, error, nodata)
        return finalizer(g) do g
            g.ptr == C_NULL || ccall(_sym(:rumi_geozl_graph_close), Cvoid, (Ptr{Cvoid},), g.ptr)
            g.ptr = C_NULL
        end
    end
end

function Base.show(io::IO, g::Graph)
    plane_word = g.planes == 1 ? "plane" : "planes"
    print(io, "Rumi.GeoZL.Graph(", g.method, ", ", g.eltype, ", row width ", g.width, ", ",
          g.planes, " ", plane_word, ", ", g.error === nothing ? "lossless" : g.error, ")")
end

"""
    graph(x, method; width=nothing, planes=nothing, error=nothing, nodata=nothing)

Build a graph for `method` from raster `x`. The graph fixes the element type,
geometry and, when lossy, the domain accepted for later tiles.

`width` defaults to `size(x, 1)`; `planes` defaults to the last dimension for
arrays with at least three dimensions. Row predictors restart at each plane.

`error` accepts a number (LINEAR), a percentage (LOG), or a full recipe.
`nothing` and zero mean lossless. For lossy compression, build the graph from
data covering the intended range.

`nodata` sets a sentinel for `missing` values. NaNs in a float raster are
detected automatically when no sentinel is given.
"""
function graph(x::AbstractArray, method::AbstractString; width=nothing, planes=nothing,
               error=nothing, nodata=nothing)
    text = _error_text(error)
    a = _samples(x, nodata)
    T = eltype(a)
    sample = nodata === nothing ? nothing : _nodata_sample(T, nodata)
    w, p = _geometry(x, width, planes)
    out = Ref{Ptr{Cvoid}}(C_NULL)
    GC.@preserve a sample begin
        _check(ccall(_sym(:rumi_geozl_graph_open), Cint,
            (Cstring, Cint, Ptr{Cvoid}, Csize_t, UInt32, UInt32, Ptr{UInt8}, Ptr{Cvoid},
             Ref{Ptr{Cvoid}}),
            method, _dtype_code(T), a, sizeof(a), w, p,
            text === nothing ? C_NULL : text, sample === nothing ? C_NULL : sample, out))
    end
    g = Graph(out[], String(method), Int(w), Int(p), T, nothing, nodata)
    # The core normalizes the short syntax, so the graph reports it as GeoZL does.
    recipe = ccall(_sym(:rumi_geozl_graph_error), Cstring, (Ptr{Cvoid},), g.ptr)
    recipe == C_NULL || (g.error = unsafe_string(recipe))
    return g
end

"""
    compress(x, g::Graph) -> Vector{UInt8}

Compress `x` with `g` and return the frame. The element type must match;
geometry is not checked. Lossy tiles outside the graph's domain are rejected.
"""
function compress(x::AbstractArray, g::Graph)
    g.ptr == C_NULL && throw(ArgumentError("this graph is closed"))
    a = _samples(x, g.nodata)
    out = Ref{Ptr{UInt8}}(C_NULL)
    out_size = Ref{Csize_t}(0)
    GC.@preserve g a begin
        _check(ccall(_sym(:rumi_geozl_compress), Cint,
            (Ptr{Cvoid}, Cint, Ptr{Cvoid}, Csize_t, Ref{Ptr{UInt8}}, Ref{Csize_t}),
            g.ptr, _dtype_code(eltype(a)), a, sizeof(a), out, out_size))
    end
    try
        return copy(unsafe_wrap(Vector{UInt8}, out[], out_size[]))
    finally
        ccall(_sym(:rumi_free), Cvoid, (Ptr{Cvoid},), out[])
    end
end

end
