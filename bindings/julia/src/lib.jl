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
    if version != 1
        Libdl.dlclose(handle)
        throw(InitError(:Rumi, ErrorException(
            "librumi C API $version is not supported; expected 1")))
    end
    _library[] = handle
end


_sym(name::Symbol) = Libdl.dlsym(_library[], name)


struct RumiError <: Exception
    message::String
end


Base.showerror(io::IO, error::RumiError) = print(io, error.message)


function _check(status::Integer)
    status == 0 && return nothing
    ptr = ccall(_sym(:rumi_last_error), Cstring, ())
    message = ptr == C_NULL ? "librumi failed with status $status" : unsafe_string(ptr)
    throw(RumiError(message))
end
