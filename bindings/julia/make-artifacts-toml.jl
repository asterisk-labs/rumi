using Downloads
using Pkg
using Pkg.Artifacts
using SHA


const REPOSITORY = "asterisk-labs/rumi"
const VERSION = isempty(ARGS) ?
    strip(read(joinpath(@__DIR__, "..", "..", "VERSION"), String)) : strip(ARGS[1])
const DIRECTORY = length(ARGS) >= 2 ? ARGS[2] : nothing
const PLATFORMS = [
    (properties = ["arch = \"x86_64\"", "os = \"linux\"", "libc = \"glibc\""],
     name = "linux-x86_64"),
    (properties = ["arch = \"aarch64\"", "os = \"macos\""],
     name = "macos-arm64"),
]
const REQUESTED = length(ARGS) >= 3 ? Set(split(ARGS[3], ',')) :
                  Set(platform.name for platform in PLATFORMS)


function hashes(name)
    asset = "librumi-$VERSION-$name.tar.gz"
    url = "https://github.com/$REPOSITORY/releases/download/v$VERSION/$asset"
    archive = DIRECTORY === nothing ? Downloads.download(url) : joinpath(DIRECTORY, asset)
    isfile(archive) || error("artifact archive does not exist: $archive")
    sha256 = bytes2hex(open(SHA.sha256, archive))
    tree = create_artifact() do destination
        Pkg.PlatformEngines.unpack(archive, destination)
    end
    return (; url, sha256, tree)
end


known = Set(platform.name for platform in PLATFORMS)
unknown = setdiff(REQUESTED, known)
isempty(unknown) || error("unknown platform: $(join(sort!(collect(unknown)), ", "))")

for platform in PLATFORMS
    platform.name in REQUESTED || continue
    info = hashes(platform.name)
    println("[[rumi]]")
    foreach(println, platform.properties)
    println("git-tree-sha1 = \"$(info.tree)\"")
    println("lazy = true\n")
    println("    [[rumi.download]]")
    println("    url = \"$(info.url)\"")
    println("    sha256 = \"$(info.sha256)\"\n")
end
