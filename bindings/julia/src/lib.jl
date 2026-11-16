# The C API version this binding is written against. A librumi built from
# another revision of rumi.h reports a different value and is refused.
const RUMI_API_VERSION = 1

const _library = Ref{Ptr{Cvoid}}(C_NULL)


function _library_names()
    Sys.iswindows() && return ("rumi.dll", "librumi.dll")
    Sys.isapple() && return ("librumi.dylib",)
    return ("librumi.so", "librumi.so.1")
end


_artifact_metadata_url(version) =
    "https://github.com/asterisk-labs/rumi/releases/download/v$version/Artifacts.toml"

_metadata_cache(version) = joinpath(first(DEPOT_PATH), "rumi", "v$version", "Artifacts.toml")


function _artifacts_toml()
    local_toml = joinpath(@__DIR__, "..", "Artifacts.toml")
    isfile(local_toml) && return local_toml, ""
    version = pkgversion(@__MODULE__)
    version === nothing && return nothing, "the package has no version"
    cached = _metadata_cache(version)
    isfile(cached) && return cached, ""
    url = _artifact_metadata_url(version)
    try
        mkpath(dirname(cached))
        mv(Downloads.download(url, cached * ".part"), cached; force=true)
    catch err
        return nothing, "could not fetch $url: $(sprint(showerror, err))"
    end
    return cached, ""
end


function _artifact_library(base)
    entries = filter(name -> !startswith(name, "."), readdir(base))
    if length(entries) == 1 && isdir(joinpath(base, entries[1]))
        base = joinpath(base, entries[1])
    end
    for name in _library_names(), directory in (base, joinpath(base, "lib"), joinpath(base, "bin"))
        path = joinpath(directory, name)
        isfile(path) && return path
    end
    return nothing
end


function _find_library()
    configured = get(ENV, "RUMI_LIB", "")
    if !isempty(configured)
        isfile(configured) || error("Rumi: RUMI_LIB=$configured does not exist")
        return configured
    end

    root = normpath(joinpath(@__DIR__, "..", "..", ".."))
    for name in _library_names()
        candidate = joinpath(root, "core", "build", name)
        isfile(candidate) && return candidate
    end

    toml, why = _artifacts_toml()
    if toml !== nothing
        if Artifacts.artifact_meta("rumi", toml) === nothing
            why = "the release has no librumi for this platform"
        else
            base = LazyArtifacts.ensure_artifact_installed("rumi", toml)
            library = _artifact_library(base)
            library === nothing || return library
            why = "the artifact holds no librumi"
        end
    end
    error("Rumi: no librumi, $why. Build it with `make build` or set RUMI_LIB")
end


function __init__()
    path = _find_library()
    handle = Libdl.dlopen(path)
    version = ccall(Libdl.dlsym(handle, :rumi_api_version), Cint, ())
    if version != RUMI_API_VERSION
        Libdl.dlclose(handle)
        throw(InitError(:Rumi, ErrorException(
            "librumi C API $version is not supported, expected $RUMI_API_VERSION")))
    end
    _library[] = handle
end


_sym(name::Symbol) = Libdl.dlsym(_library[], name)


# Status codes from rumi.h, except RUMI_ERR_INVALID, which throws ArgumentError.
const _ERROR_KINDS = Dict{Int,Symbol}(
    2 => :io,
    3 => :parse,
    4 => :format,
    5 => :decode,
    6 => :oom,
    7 => :unsupported,
    8 => :state,
    99 => :internal,
)

"""
    RumiError(kind, message)

A `librumi` failure other than an invalid request. `kind` names the status as
one of `:io`, `:parse`, `:format`, `:decode`, `:oom`, `:unsupported`, `:state`
or `:internal`. `RumiError(message)` is an `:internal` failure of the binding.
"""
struct RumiError <: Exception
    kind::Symbol
    message::String

    function RumiError(kind::Symbol, message::AbstractString)
        kind in values(_ERROR_KINDS) ||
            throw(ArgumentError("$(repr(kind)) is not a Rumi error kind"))
        return new(kind, String(message))
    end
end

RumiError(message::AbstractString) = RumiError(:internal, message)

Base.showerror(io::IO, error::RumiError) =
    print(io, "RumiError(", repr(error.kind), "): ", error.message)


const _ERR_INVALID = 1

function _check(status::Integer)
    status == 0 && return nothing
    ptr = ccall(_sym(:rumi_last_error), Cstring, ())
    message = ptr == C_NULL ? "librumi failed with status $status" : unsafe_string(ptr)
    status == _ERR_INVALID && throw(ArgumentError(message))
    throw(RumiError(get(_ERROR_KINDS, Int(status), :internal), message))
end


"""
    set_num_threads(n) -> Int

Set the process-wide read thread count, a whole number from 1 to 1024. Set
it before the first parallel read. Once the pool exists, a different count
throws `RumiError` of kind `:state`. `RUMI_NUM_THREADS` gives the initial
value and also accepts `ALL_CPUS`. Returns `n`.
"""
function set_num_threads(n)
    n isa Integer && !(n isa Bool) ||
        throw(ArgumentError("num_threads must be an integer, got $(repr(n))"))
    typemin(Cint) <= n <= typemax(Cint) ||
        throw(ArgumentError("num_threads must be in [1, 1024]"))
    _check(ccall(_sym(:rumi_set_num_threads), Cint, (Cint,), n))
    return Int(n)
end

"""
    get_num_threads() -> Int

Return the process-wide read thread count. It is the configured count before
the first parallel read and the pool's fixed size afterwards.
"""
function get_num_threads()
    out = Ref{Cint}(0)
    _check(ccall(_sym(:rumi_get_num_threads), Cint, (Ref{Cint},), out))
    return Int(out[])
end

"""
    set_checksum_verification(on::Bool) -> Bool

Turn process-wide OpenZL checksum verification on or off. Set it before the
first decoded frame. Afterwards, a different value throws `RumiError` of kind
`:state`. `RUMI_VERIFY` gives the initial value. Returns `on`.
"""
function set_checksum_verification(on)
    on isa Bool ||
        throw(ArgumentError("checksum verification must be a Bool, got $(repr(on))"))
    _check(ccall(_sym(:rumi_set_checksum_verification), Cint, (Cint,), on))
    return on
end

"""
    get_checksum_verification() -> Bool

Return whether OpenZL checksum verification is on. It is the configured value
before the first decoded frame and the fixed value afterwards.
"""
function get_checksum_verification()
    out = Ref{Cint}(0)
    _check(ccall(_sym(:rumi_get_checksum_verification), Cint, (Ref{Cint},), out))
    return out[] != 0
end
