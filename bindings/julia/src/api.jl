function _parse_header(header::Vector{UInt8})
    out = Ref{Ptr{Cvoid}}(C_NULL)
    GC.@preserve header _check(ccall(
        _sym(:rumi_spec_parse), Cint,
        (Ptr{UInt8}, Csize_t, Ref{Ptr{Cvoid}}),
        header, length(header), out,
    ))
    return out[]
end

function _open_source(source::AbstractString)
    out = Ref{Ptr{Cvoid}}(C_NULL)
    _check(ccall(
        _sym(:rumi_source_file), Cint,
        (Cstring, Ref{Ptr{Cvoid}}), source, out,
    ))
    return out[]
end

function _unit_name(header::_Header)
    buffer = zeros(UInt8, 16)
    _check(ccall(
        _sym(:rumi_unit_name), Cint,
        (UInt8, UInt16, UInt32, Ptr{UInt8}, Csize_t),
        header.frame_unit, header.samples_per_pixel, header.time_count,
        buffer, length(buffer),
    ))
    first_zero = something(findfirst(iszero, buffer), length(buffer) + 1)
    return String(buffer[1:first_zero-1])
end

function _index_order(header::_Header)
    axes = zeros(UInt8, 2)
    ndim = Ref{Cint}(0)
    _check(ccall(
        _sym(:rumi_unit_index_axes), Cint,
        (UInt8, UInt16, UInt32, Ptr{UInt8}, Ref{Cint}),
        header.frame_unit, header.samples_per_pixel, header.time_count,
        axes, ndim,
    ))
    names = Symbol[]
    for axis in axes[1:ndim[]]
        ptr = ccall(_sym(:rumi_axis_name), Cstring, (UInt8,), axis)
        push!(names, Symbol(unsafe_string(ptr)))
    end
    return Tuple(names)
end

function _metadata(source::Ptr{Cvoid}, header::Vector{UInt8})
    native = Ref{_Metadata}()
    GC.@preserve header _check(ccall(
        _sym(:rumi_info), Cint,
        (Ptr{Cvoid}, Ptr{UInt8}, Csize_t, Ref{_Metadata}),
        source, header, length(header), native,
    ))
    value = native[]
    try
        h = value.fields
        canonical_header = copy(unsafe_wrap(Vector{UInt8}, value.blob, value.blob_size))
        shape = h.time_count > 1 ?
            (Int(h.time_count), Int(h.samples_per_pixel), Int(h.image_length), Int(h.image_width)) :
            (Int(h.samples_per_pixel), Int(h.image_length), Int(h.image_width))
        _, dtype_name = _dtype(h.dtype)
        bands = [unsafe_string(unsafe_load(value.band_texts, i))
                 for i in 1:Int(value.band_text_count)]
        coordinates = [unsafe_load(value.time, i) for i in 1:Int(value.time_coords)]
        times, kind = if value.time_type == 2
            [Dates.unix2datetime(second) for second in coordinates], :instant
        elseif value.time_type == 1
            [(Dates.unix2datetime(coordinates[i]), Dates.unix2datetime(coordinates[i + 1]))
             for i in 1:2:length(coordinates)], :interval
        else
            throw(RumiError("librumi returned invalid time type $(value.time_type)"))
        end
        transform = value.epsg == 0 ? nothing : NTuple{6,Float64}(value.transform)
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
            value.epsg == 0 ? nothing : Int(value.epsg),
            Bool(value.pixel_is_point),
        )
    finally
        ccall(_sym(:rumi_metadata_free), Cvoid, (Ref{_Metadata},), native)
    end
end

"""
    open(source, header) -> Dataset

Pair a local path or remote URI with its external header. Opening validates
the pair and reads the transform, CRS, band descriptions and time axis.
"""
function open(source::AbstractString, header::AbstractVector{UInt8})
    owned_header = Vector{UInt8}(header)
    spec = _parse_header(owned_header)
    native_source = C_NULL
    try
        native_source = _open_source(expanduser(source))
        metadata = _metadata(native_source, owned_header)
        file = Dataset(native_source, spec, metadata)
        finalizer(close, file)
        return file
    catch
        native_source == C_NULL || ccall(
            _sym(:rumi_source_free), Cvoid, (Ptr{Cvoid},), native_source)
        ccall(_sym(:rumi_spec_destroy), Cvoid, (Ptr{Cvoid},), spec)
        rethrow()
    end
end

function _header(file::Dataset)
    isopen(file) || throw(ArgumentError("this Rumi Dataset is closed"))
    result = Ref{_Header}()
    _check(ccall(
        _sym(:rumi_spec_header), Cint,
        (Ptr{Cvoid}, Ref{_Header}), file.spec, result,
    ))
    return result[]
end

function _selection(selection, total::Integer, name::String)
    selection === nothing && return Cint[]
    values = selection isa Integer ? [selection] : collect(selection)
    isempty(values) && throw(ArgumentError("$name cannot be empty"))
    out = Cint[]
    for value in values
        value isa Integer && !(value isa Bool) ||
            throw(ArgumentError("every $name index must be an integer"))
        1 <= value <= total ||
            throw(ArgumentError("$name index $value is outside 1:$total"))
        value <= typemax(Cint) ||
            throw(ArgumentError("$name index $value exceeds the C API limit"))
        push!(out, Cint(value))
    end
    return out
end

function _window(window, header::_Header)
    window === nothing && return (0, Int(header.image_length), 0, Int(header.image_width))
    length(window) == 4 ||
        throw(ArgumentError("window must be (row, column, height, width)"))
    row, column, height, width = window
    all(value -> value isa Integer && !(value isa Bool), window) ||
        throw(ArgumentError("window values must be integers"))
    row >= 1 && column >= 1 && height >= 1 && width >= 1 ||
        throw(ArgumentError("window uses one-based origins and positive sizes"))
    all(value -> value <= typemax(Cint), window) ||
        throw(ArgumentError("window values exceed the C API limit"))
    row + height - 1 <= header.image_length &&
        column + width - 1 <= header.image_width ||
        throw(ArgumentError("window extends past the image"))
    return (Int(row - 1), Int(height), Int(column - 1), Int(width))
end

function _layout(pattern, n, t, b, y, x)
    result = Ref{_Layout}()
    status = if pattern === nothing
        ccall(_sym(:rumi_compile_layout), Cint,
              (Ptr{UInt8}, Int64, Int64, Int64, Int64, Int64, Ref{_Layout}),
              C_NULL, n, t, b, y, x, result)
    else
        pattern isa AbstractString || throw(ArgumentError("pattern must be a string or nothing"))
        ccall(_sym(:rumi_compile_layout), Cint,
              (Cstring, Int64, Int64, Int64, Int64, Int64, Ref{_Layout}),
              pattern, n, t, b, y, x, result)
    end
    _check(status)
    return result[]
end

function _output_pattern(pattern, n_items, file_times)
    pattern === nothing || return pattern
    ptr = ccall(
        _sym(:rumi_default_pattern), Cstring,
        (Csize_t, UInt32), n_items, file_times,
    )
    ptr == C_NULL && throw(RumiError("librumi returned no default output pattern"))
    return unsafe_string(ptr)
end

function _array(header::_Header, layout::_Layout)
    T, _ = _dtype(header.dtype)
    shape = ntuple(i -> Int(layout.shape[i]), Int(layout.ndim))
    return Array{T}(undef, reverse(shape))
end

function read(file::Dataset; time=nothing, bands=nothing, window=nothing, pattern=nothing)
    header = _header(file)
    selected_time = _selection(time, header.time_count, "time")
    selected_bands = _selection(bands, header.samples_per_pixel, "bands")
    y_off, y_size, x_off, x_size = _window(window, header)
    nt = isempty(selected_time) ? Int(header.time_count) : length(selected_time)
    nb = isempty(selected_bands) ? Int(header.samples_per_pixel) : length(selected_bands)
    output_pattern = _output_pattern(pattern, 1, header.time_count)
    layout = _layout(output_pattern, 1, nt, nb, y_size, x_size)
    out = _array(header, layout)
    GC.@preserve selected_time selected_bands out begin
        time_ptr = isempty(selected_time) ? Ptr{Cint}(C_NULL) : pointer(selected_time)
        band_ptr = isempty(selected_bands) ? Ptr{Cint}(C_NULL) : pointer(selected_bands)
        _check(ccall(
            _sym(:rumi_read), Cint,
            (Ptr{Cvoid}, Ptr{Cvoid}, Ptr{Cint}, Csize_t, Ptr{Cint}, Csize_t,
             Cint, Cint, Cint, Cint, Ptr{UInt8}, Ptr{Cvoid}, Csize_t),
            file.source, file.spec,
            time_ptr, length(selected_time), band_ptr, length(selected_bands),
            y_off, y_size, x_off, x_size, output_pattern, out, sizeof(out),
        ))
    end
    return out
end

function read(source::AbstractString, header::AbstractVector{UInt8}; kwargs...)
    file = open(source, header)
    try
        return read(file; kwargs...)
    finally
        close(file)
    end
end

function read_many(files::AbstractVector{<:Dataset}; windows=nothing,
                   time=nothing, bands=nothing, pattern=nothing)
    isempty(files) && throw(ArgumentError("read_many needs at least one Dataset"))
    headers = _header.(files)
    base = headers[1]
    selected_time = _selection(time, base.time_count, "time")
    selected_bands = _selection(bands, base.samples_per_pixel, "bands")
    raw_windows = windows === nothing ? fill(nothing, length(files)) : collect(windows)
    length(raw_windows) == length(files) ||
        throw(ArgumentError("windows must have one entry per Dataset"))
    resolved = [_window(window, header) for (window, header) in zip(raw_windows, headers)]
    y_size, x_size = resolved[1][2], resolved[1][4]
    all(window -> (window[2], window[4]) == (y_size, x_size), resolved) ||
        throw(ArgumentError("every window must have the same height and width"))
    items = [_ReadItem(file.source, file.spec, window[1], window[3])
             for (file, window) in zip(files, resolved)]
    nt = isempty(selected_time) ? Int(base.time_count) : length(selected_time)
    nb = isempty(selected_bands) ? Int(base.samples_per_pixel) : length(selected_bands)
    output_pattern = _output_pattern(pattern, length(files), base.time_count)
    layout = _layout(output_pattern, length(files), nt, nb, y_size, x_size)
    out = _array(base, layout)
    GC.@preserve items selected_time selected_bands out begin
        time_ptr = isempty(selected_time) ? Ptr{Cint}(C_NULL) : pointer(selected_time)
        band_ptr = isempty(selected_bands) ? Ptr{Cint}(C_NULL) : pointer(selected_bands)
        _check(ccall(
            _sym(:rumi_read_many), Cint,
            (Ptr{_ReadItem}, Csize_t, Ptr{Cint}, Csize_t, Ptr{Cint}, Csize_t,
             Cint, Cint, Ptr{UInt8}, Ptr{Cvoid}, Csize_t),
            items, length(items), time_ptr, length(selected_time),
            band_ptr, length(selected_bands), y_size, x_size,
            output_pattern, out, sizeof(out),
        ))
    end
    return out
end

function read_many(sources::AbstractVector{<:AbstractString},
                   headers::AbstractVector{<:AbstractVector{UInt8}}; kwargs...)
    length(sources) == length(headers) ||
        throw(ArgumentError("sources and headers must have the same length"))
    files = Dataset[]
    try
        for (source, header) in zip(sources, headers)
            push!(files, open(source, header))
        end
        return read_many(files; kwargs...)
    finally
        close.(files)
    end
end
