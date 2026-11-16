function _parse_header(header::Vector{UInt8})
    out = Ref{Ptr{Cvoid}}(C_NULL)
    GC.@preserve header _check(ccall(
        _sym(:rumi_spec_parse), Cint,
        (Ptr{UInt8}, Csize_t, Ref{Ptr{Cvoid}}),
        header, length(header), out,
    ))
    return out[]
end

# A path keeps its text with `~` expanded. Bytes are borrowed as given, which
# suits a call that frees the native source before it returns.
function _source_value(source)
    source isa AbstractString && return String(expanduser(source))
    source isa AbstractVector{UInt8} ||
        throw(ArgumentError("source must be a path, a URI or the file's bytes, " *
                            "got $(typeof(source))"))
    return source isa Vector{UInt8} ? source : Vector{UInt8}(source)
end

function _header_value(header)
    header isa AbstractVector{UInt8} ||
        throw(ArgumentError("header must be an AbstractVector{UInt8}, got $(typeof(header))"))
    return Vector{UInt8}(header)
end

function _open_source(source::String)
    out = Ref{Ptr{Cvoid}}(C_NULL)
    _check(ccall(
        _sym(:rumi_source_file), Cint,
        (Cstring, Ref{Ptr{Cvoid}}), source, out,
    ))
    return out[]
end

# The native source borrows the bytes, so their owner must outlive it and must
# not resize them.
function _open_source(source::Union{Vector{UInt8},Base.CodeUnits{UInt8,String}})
    out = Ref{Ptr{Cvoid}}(C_NULL)
    _check(ccall(
        _sym(:rumi_source_memory), Cint,
        (Ptr{UInt8}, Csize_t, Ref{Ptr{Cvoid}}), source, length(source), out,
    ))
    return out[]
end

_free_source(source::Ptr{Cvoid}) =
    ccall(_sym(:rumi_source_free), Cvoid, (Ptr{Cvoid},), source)

_free_spec(spec::Ptr{Cvoid}) = ccall(_sym(:rumi_spec_destroy), Cvoid, (Ptr{Cvoid},), spec)

function _unit_name(unit::Integer, bands::Integer, times::Integer)
    buffer = zeros(UInt8, 16)
    _check(ccall(
        _sym(:rumi_unit_name), Cint,
        (UInt8, UInt16, UInt32, Ptr{UInt8}, Csize_t),
        unit, bands, times, buffer, length(buffer),
    ))
    first_zero = something(findfirst(iszero, buffer), length(buffer) + 1)
    return String(buffer[1:first_zero-1])
end

_unit_name(header::_Header) =
    _unit_name(header.frame_unit, header.samples_per_pixel, header.time_count)

function _index_order(unit::Integer, bands::Integer, times::Integer)
    axes = zeros(UInt8, 2)
    ndim = Ref{Cint}(0)
    _check(ccall(
        _sym(:rumi_unit_index_axes), Cint,
        (UInt8, UInt16, UInt32, Ptr{UInt8}, Ref{Cint}),
        unit, bands, times, axes, ndim,
    ))
    names = Symbol[]
    for axis in axes[1:ndim[]]
        ptr = ccall(_sym(:rumi_axis_name), Cstring, (UInt8,), axis)
        push!(names, Symbol(unsafe_string(ptr)))
    end
    return Tuple(names)
end

_index_order(header::_Header) =
    _index_order(header.frame_unit, header.samples_per_pixel, header.time_count)

# Whole UTC days read as dates, as in the Python binding. Another writer may
# store a time no binding's date type holds, so it is refused, not wrapped.
function _time_values(value::_Metadata)
    seconds = [unsafe_load(value.time, i) for i in 1:Int(value.time_coords)]
    for (i, second) in enumerate(seconds)
        _FIRST_SECOND <= second <= _LAST_SECOND && continue
        step = value.time_type == _INTERVAL ? (i + 1) ÷ 2 : i
        throw(RumiError(:format,
                        "the $(_ordinal(step)) time step falls outside the years 1 to 9999"))
    end
    steps = all(second -> mod(second, 86400) == 0, seconds) ?
        [Date(1970) + Day(fld(second, 86400)) for second in seconds] :
        [Dates.unix2datetime(second) for second in seconds]
    value.time_type == _INSTANT && return steps, :instant
    value.time_type == _INTERVAL &&
        return [(steps[i], steps[i + 1]) for i in 1:2:length(steps)], :interval
    throw(RumiError("librumi returned invalid time type $(value.time_type)"))
end

# A null source inspects the header alone, and a null header the source alone.
function _metadata(source::Ptr{Cvoid}, header::Union{Nothing,Vector{UInt8}})
    native = Ref{_Metadata}()
    GC.@preserve header _check(ccall(
        _sym(:rumi_info), Cint,
        (Ptr{Cvoid}, Ptr{UInt8}, Csize_t, Ref{_Metadata}),
        source, header === nothing ? C_NULL : pointer(header),
        header === nothing ? 0 : length(header), native,
    ))
    value = native[]
    try
        h = value.fields
        canonical_header = copy(unsafe_wrap(Vector{UInt8}, value.blob, value.blob_size))
        shape = h.time_count > 1 ?
            (Int(h.time_count), Int(h.samples_per_pixel), Int(h.image_length), Int(h.image_width)) :
            (Int(h.samples_per_pixel), Int(h.image_length), Int(h.image_width))
        _, dtype_name = _dtype(h.dtype)
        bands = times = kind = transform = crs = pixel_is_point = nothing
        if value.has_source != 0
            bands = [unsafe_string(unsafe_load(value.band_texts, i))
                     for i in 1:Int(value.band_text_count)]
            times, kind = _time_values(value)
            if value.epsg != 0
                transform = NTuple{6,Float64}(value.transform)
                crs = Int(value.epsg)
            end
            pixel_is_point = value.pixel_is_point != 0
        end
        frames = Int(h.tiles_across) * Int(h.tiles_down)
        order = _index_order(h)
        :b in order && (frames *= Int(h.samples_per_pixel))
        :t in order && (frames *= Int(h.time_count))
        return Metadata(
            canonical_header,
            shape,
            Int(h.time_count),
            dtype_name,
            (Int(h.tile_length), Int(h.tile_width)),
            _unit_name(h),
            order,
            frames,
            bands,
            times,
            kind,
            transform,
            crs,
            pixel_is_point,
        )
    finally
        ccall(_sym(:rumi_metadata_free), Cvoid, (Ref{_Metadata},), native)
    end
end

"""
    open(source, header) -> Dataset
    open(f, source, header)

Pair a source with its external header. `source` is a local path, a remote
URI or the file's bytes as an `AbstractVector{UInt8}`. Opening checks that the
header matches the source and reads the transform, CRS, band descriptions and
time axis. The second form passes the Dataset to `f` and closes it when `f`
returns.
"""
function open(source, header)
    source isa Dataset && throw(ArgumentError("header is already carried by this Dataset"))
    # An immutable copy, so nothing reached through the Dataset can resize or
    # rewrite the bytes the native source borrows.
    owned = source isa AbstractVector{UInt8} ? String(Vector{UInt8}(source)) : nothing
    value = owned === nothing ? _source_value(source) : codeunits(owned)
    owned_header = _header_value(header)
    native_source = _open_source(value)
    spec = C_NULL
    try
        spec = _parse_header(owned_header)
        metadata = GC.@preserve owned _metadata(native_source, owned_header)
        return finalizer(close, Dataset(value, owned_header, metadata, native_source, spec, owned))
    catch
        _free_source(native_source)
        spec == C_NULL || _free_spec(spec)
        rethrow()
    end
end

function open(f::Function, source, header)
    file = open(source, header)
    try
        return f(file)
    finally
        close(file)
    end
end

"""
    info(source; header=nothing) -> Metadata
    info(; header) -> Metadata

Inspect a source, an external header, or both. `source` is a local path, a
remote URI, the file's bytes or a [`Dataset`](@ref). A source alone is read
and its canonical header rebuilt. A header alone opens nothing, and leaves
`bands`, `time`, `time_kind`, `transform`, `crs` and `pixel_is_point` as
`nothing`. Given both, the header must match the source. A Dataset returns
its metadata and takes no header.
"""
function info(source=nothing; header=nothing)
    if source isa Dataset
        header === nothing || throw(ArgumentError("header is already carried by this Dataset"))
        return source.metadata
    end
    owned_header = header === nothing ? nothing : _header_value(header)
    source === nothing && return _metadata(Ptr{Cvoid}(C_NULL), owned_header)
    value = _source_value(source)
    native_source = _open_source(value)
    try
        return GC.@preserve value _metadata(native_source, owned_header)
    finally
        _free_source(native_source)
    end
end

function _header(spec::Ptr{Cvoid})
    result = Ref{_Header}()
    _check(ccall(
        _sym(:rumi_spec_header), Cint,
        (Ptr{Cvoid}, Ref{_Header}), spec, result,
    ))
    return result[]
end

function _header(file::Dataset)
    isopen(file) || throw(ArgumentError("this Rumi Dataset is closed"))
    return _header(file._spec)
end

# A tuple is refused so that (1, 3) never means two indices here and a range
# in another binding.
function _selection(selection, name::String, total::Integer)
    selection === nothing && return nothing
    selection isa Tuple &&
        throw(ArgumentError("$name takes a list or a range, not a tuple"))
    values = if selection isa Integer
        [selection]
    elseif selection isa AbstractRange
        # One entry past the axis is enough for the core to refuse a long range.
        first(selection, Int(total) + 1)
    elseif selection isa AbstractVector
        selection
    else
        throw(ArgumentError("$name takes an integer, a list or a range"))
    end
    out = Cint[]
    for value in values
        value isa Integer && !(value isa Bool) ||
            throw(ArgumentError("every $name index must be an integer"))
        typemin(Cint) <= value <= typemax(Cint) ||
            throw(ArgumentError("$name index $value exceeds the C API limit"))
        push!(out, Cint(value))
    end
    return out
end

# A null pointer selects all indices; an empty vector must stay non-null.
_c_selection(values) = values === nothing ? Ptr{Cint}(C_NULL) :
    isempty(values) ? Ref{Cint}(0) : values

_c_length(values) = values === nothing ? 0 : length(values)

function _window(window, header::_Header)
    window === nothing && return (0, Int(header.image_length), 0, Int(header.image_width))
    window isa Union{Tuple,AbstractVector} && length(window) == 4 ||
        throw(ArgumentError("window must be (row, column, height, width)"))
    row, column, height, width = window
    all(value -> value isa Integer && !(value isa Bool), window) ||
        throw(ArgumentError("window values must be integers"))
    all(value -> typemin(Cint) < value <= typemax(Cint), window) ||
        throw(ArgumentError("window values exceed the C API limit"))
    return (Int(row - 1), Int(height), Int(column - 1), Int(width))
end

# One window per item, as a vector or as the rows of an n-by-4 matrix.
function _windows(windows, count::Integer)
    windows === nothing && return fill(nothing, count)
    entries = if windows isa AbstractMatrix
        size(windows, 2) == 4 || throw(ArgumentError(
            "a windows matrix holds one (row, column, height, width) window per row"))
        [windows[i, :] for i in axes(windows, 1)]
    elseif windows isa AbstractVector
        windows
    else
        throw(ArgumentError(
            "windows is a vector with one window per Dataset, or a matrix with one per row"))
    end
    length(entries) == count ||
        throw(ArgumentError("windows must have one entry per Dataset"))
    return entries
end

function _pattern(pattern)
    pattern === nothing || pattern isa AbstractString ||
        throw(ArgumentError("pattern must be a string or nothing"))
    return pattern === nothing ? C_NULL : pattern
end

function _array(header::_Header, layout::_Layout)
    T, _ = _dtype(header.dtype)
    shape = ntuple(i -> Int(layout.shape[i]), Int(layout.ndim))
    return Array{T}(undef, reverse(shape))
end

"""
    read(file::Dataset; time=nothing, bands=nothing, window=nothing, pattern=nothing) -> Array
    read(source, header; kwargs...) -> Array

Read a selection into a new array of the file's element type. `time` and
`bands` take a one-based index, a vector of them or a range, and `nothing`
for all. `window` is `(row, column, height, width)` with a one-based origin.
The second form opens `source` with `header`, reads it and closes it. It skips
the check [`open`](@ref) makes that the header matches the source.
"""
function read(file::Dataset; time=nothing, bands=nothing, window=nothing, pattern=nothing)
    isopen(file) || throw(ArgumentError("this Rumi Dataset is closed"))
    return GC.@preserve file _read(file._source, file._spec, time, bands, window, pattern)
end

# Like Python's read, a one-shot read skips rumi_info, which would fetch the
# index and trailer before the frames.
function read(source, header; time=nothing, bands=nothing, window=nothing, pattern=nothing)
    source isa Dataset && throw(ArgumentError("header is already carried by this Dataset"))
    value = _source_value(source)
    owned_header = _header_value(header)
    native_source = _open_source(value)
    spec = C_NULL
    try
        spec = _parse_header(owned_header)
        return GC.@preserve value _read(native_source, spec, time, bands, window, pattern)
    finally
        _free_source(native_source)
        spec == C_NULL || _free_spec(spec)
    end
end

# The caller keeps the source's bytes alive.
function _read(source::Ptr{Cvoid}, spec::Ptr{Cvoid}, time, bands, window, pattern)
    header = _header(spec)
    selected_time = _selection(time, "time", header.time_count)
    selected_bands = _selection(bands, "bands", header.samples_per_pixel)
    y_off, y_size, x_off, x_size = _window(window, header)
    output_pattern = _pattern(pattern)
    nt, nb = _c_length(selected_time), _c_length(selected_bands)
    layout = Ref{_Layout}()
    _check(ccall(
        _sym(:rumi_read_layout), Cint,
        (Ptr{Cvoid}, Ptr{Cint}, Csize_t, Ptr{Cint}, Csize_t,
         Cint, Cint, Cint, Cint, Ptr{UInt8}, Ref{_Layout}, Ptr{Csize_t}),
        spec, _c_selection(selected_time), nt, _c_selection(selected_bands), nb,
        y_off, y_size, x_off, x_size, output_pattern, layout, C_NULL,
    ))
    out = _array(header, layout[])
    _check(ccall(
        _sym(:rumi_read), Cint,
        (Ptr{Cvoid}, Ptr{Cvoid}, Ptr{Cint}, Csize_t, Ptr{Cint}, Csize_t,
         Cint, Cint, Cint, Cint, Ptr{UInt8}, Ptr{Cvoid}, Csize_t),
        source, spec,
        _c_selection(selected_time), nt, _c_selection(selected_bands), nb,
        y_off, y_size, x_off, x_size, output_pattern, out, sizeof(out),
    ))
    return out
end

"""
    read_many(files::AbstractVector{Dataset}; windows=nothing, time=nothing,
              bands=nothing, pattern=nothing) -> Array
    read_many(sources, headers; kwargs...) -> Array

Read one window from each Dataset into one array with the item axis last.
`windows` holds one window or `nothing` per Dataset, or is an n-by-4 matrix
with one window per row. Without it every whole image is read. The second
form opens each source with its header, reads them and closes them.
"""
function read_many(files; windows=nothing, time=nothing, bands=nothing, pattern=nothing)
    files isa AbstractVector && all(file -> file isa Dataset, files) || throw(ArgumentError(
        "read_many takes a vector of Datasets, or sources with one header each"))
    all(isopen, files) || throw(ArgumentError("this Rumi Dataset is closed"))
    sources = Ptr{Cvoid}[file._source for file in files]
    specs = Ptr{Cvoid}[file._spec for file in files]
    return GC.@preserve files _read_many(sources, specs, windows, time, bands, pattern)
end

function read_many(sources, headers; windows=nothing, time=nothing, bands=nothing,
                   pattern=nothing)
    sources isa AbstractVector && !(sources isa AbstractVector{UInt8}) ||
        throw(ArgumentError("read_many takes a vector of sources; use read for one"))
    headers isa AbstractVector && !(headers isa AbstractVector{UInt8}) ||
        throw(ArgumentError("read_many needs one header per source"))
    any(source -> source isa Dataset, sources) &&
        throw(ArgumentError("headers are already carried by these Datasets"))
    length(sources) == length(headers) ||
        throw(ArgumentError("sources and headers must have the same length"))
    values = [_source_value(source) for source in sources]
    owned_headers = [_header_value(header) for header in headers]
    native_sources, specs = Ptr{Cvoid}[], Ptr{Cvoid}[]
    try
        for (value, header) in zip(values, owned_headers)
            push!(native_sources, _open_source(value))
            push!(specs, _parse_header(header))
        end
        return GC.@preserve values _read_many(
            native_sources, specs, windows, time, bands, pattern)
    finally
        foreach(_free_source, native_sources)
        foreach(_free_spec, specs)
    end
end

# The caller keeps every source's bytes alive.
function _read_many(sources::Vector{Ptr{Cvoid}}, specs::Vector{Ptr{Cvoid}},
                    windows, time, bands, pattern)
    isempty(specs) && throw(ArgumentError("read_many needs at least one item"))
    headers = [_header(spec) for spec in specs]
    selected_time = _selection(time, "time", headers[1].time_count)
    selected_bands = _selection(bands, "bands", headers[1].samples_per_pixel)
    resolved = [_window(window, header)
                for (window, header) in zip(_windows(windows, length(specs)), headers)]
    y_size, x_size = resolved[1][2], resolved[1][4]
    all(window -> (window[2], window[4]) == (y_size, x_size), resolved) ||
        throw(ArgumentError("every window must have the same height and width"))
    items = [_ReadItem(source, spec, window[1], window[3])
             for (source, spec, window) in zip(sources, specs, resolved)]
    output_pattern = _pattern(pattern)
    nt, nb = _c_length(selected_time), _c_length(selected_bands)
    layout = Ref{_Layout}()
    _check(ccall(
        _sym(:rumi_read_many_layout), Cint,
        (Ptr{_ReadItem}, Csize_t, Ptr{Cint}, Csize_t, Ptr{Cint}, Csize_t,
         Cint, Cint, Ptr{UInt8}, Ref{_Layout}, Ptr{Csize_t}),
        items, length(items),
        _c_selection(selected_time), nt, _c_selection(selected_bands), nb,
        y_size, x_size, output_pattern, layout, C_NULL,
    ))
    out = _array(headers[1], layout[])
    _check(ccall(
        _sym(:rumi_read_many), Cint,
        (Ptr{_ReadItem}, Csize_t, Ptr{Cint}, Csize_t, Ptr{Cint}, Csize_t,
         Cint, Cint, Ptr{UInt8}, Ptr{Cvoid}, Csize_t),
        items, length(items),
        _c_selection(selected_time), nt, _c_selection(selected_bands), nb,
        y_size, x_size, output_pattern, out, sizeof(out),
    ))
    return out
end
