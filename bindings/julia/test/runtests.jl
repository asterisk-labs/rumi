using Dates
using Rumi
using Test

path = get(ENV, "RUMI_TEST_FILE", joinpath(@__DIR__, "fixtures", "binding.rumi"))
header_path = get(ENV, "RUMI_TEST_HEADER", joinpath(@__DIR__, "fixtures", "binding.header"))
float_path = joinpath(@__DIR__, "fixtures", "binding-f64.rumi")
float_header_path = joinpath(@__DIR__, "fixtures", "binding-f64.header")

@testset "package API" begin
    @test Rumi.open !== Base.open
    @test Rumi.read !== Base.read
    @test Rumi.write !== Base.write
    @test :open ∉ names(Rumi)
    @test :read ∉ names(Rumi)
    @test :write ∉ names(Rumi)
    @test sizeof(Rumi.RumiBFloat16) == 2
end

@testset "fixture reads" begin
    @test isfile(path)
    @test isfile(header_path)
    @test isfile(float_path)
    @test isfile(float_header_path)
    header = Base.read(header_path)
    file = Rumi.open(path, header)
    metadata = Rumi.info(file)

    @test isopen(file)
    @test metadata.shape == (2, 3, 6, 8)
    @test metadata.time_count == 2
    @test metadata.dtype == "uint16"
    @test metadata.bands == ["B02", "B03", "B04"]
    @test metadata.crs == 32618
    @test metadata.transform == (10.0, 0.0, 500000.0, 0.0, -10.0, 8000000.0)
    @test metadata.time_kind == :instant
    @test length(metadata.time) == 2

    whole = Rumi.read(file)
    @test size(whole) == (8, 6, 3, 2)
    @test whole[1, 1, 1, 1] == 0
    @test whole[8, 6, 3, 2] == 12507

    chip = Rumi.read(file; time=[2, 1], bands=[3, 1], window=(2, 3, 3, 4))
    @test size(chip) == (4, 3, 2, 2)
    @test chip[1, 1, 1, 1] == 12102
    @test chip[4, 3, 2, 2] == 305

    reordered = Rumi.read(
        file;
        time=[1], bands=[2, 3], window=(1, 1, 2, 3),
        pattern="t y x b",
    )
    @test size(reordered) == (2, 3, 2, 1)
    @test reordered[1, 1, 1, 1] == 1000
    @test reordered[2, 3, 2, 1] == 2102

    direct = Rumi.read(path, header; time=[2], bands=[1], window=(1, 1, 1, 1))
    @test size(direct) == (1, 1, 1, 1)
    @test only(direct) == 10000
    withenv("HOME" => dirname(path)) do
        expanded = Rumi.open(joinpath("~", basename(path)), header)
        @test size(Rumi.read(expanded; window=(1, 1, 1, 1))) == (1, 1, 3, 2)
        close(expanded)
    end
    @test_throws ArgumentError("bands selects a band before the first") Rumi.read(file; bands=[0])
    @test_throws ArgumentError("bands selects no band") Rumi.read(file; bands=Int[])
    @test_throws "the window extends past the image" Rumi.read(file; window=(5, 1, 4, 1))
    @test_throws "every window must have the same height" Rumi.read_many(
        [file, file]; windows=[(1, 1, 2, 2), (1, 1, 1, 1)])
    @test size(Rumi.read_many([file]; windows=[(1, 1, 2, 3)])) == (3, 2, 3, 2, 1)
    bad_header = copy(header)
    bad_header[1] = xor(bad_header[1], 0x01)
    @test_throws RumiError Rumi.open(path, bad_header)

    batch = Rumi.read_many(
        [file, file];
        windows=[(1, 1, 2, 3), (3, 4, 2, 3)],
        bands=[2],
    )
    @test size(batch) == (3, 2, 1, 2, 2)
    @test batch[:, :, :, :, 1] == whole[1:3, 1:2, 2:2, :]
    @test batch[:, :, :, :, 2] == whole[4:6, 3:4, 2:2, :]

    floating = Rumi.open(float_path, Base.read(float_header_path))
    float_chip = Rumi.read(floating; time=[2], bands=[3], window=(2, 3, 3, 4))
    @test eltype(float_chip) == Float64
    @test size(float_chip) == (4, 3, 1, 1)
    @test float_chip[1, 1, 1, 1] == 1210.2
    close(floating)

    close(file)
    @test !isopen(file)
    @test_throws ArgumentError Rumi.read(file)
end

# An Image of 5 columns, 3 rows and 2 bands, as Julia holds it, (x, y, band).
image = reshape(UInt16.(0:29), 5, 3, 2)
const LAYOUT = "b (row h) (col w) -> row col b (h w)"

@testset "frames" begin
    table = Rumi.frames(image, LAYOUT; tile_size=2)
    @test table isa FrameTable{UInt16,2}
    @test table.layout == "h w"
    @test table.dtype == "uint16"
    @test length(table) == 12
    @test all(isnothing, table.compressed)
    @test table.data[1] == image[1:2, 1:2, 1]
    @test table.data[2] == image[1:2, 1:2, 2]
    @test table.data[5] == reshape(image[5, 1:2, 1], 1, 2)
    @test table.data[12] == reshape(image[5:5, 3:3, 2], 1, 1)
    @test sprint(show, table) ==
          "Rumi.FrameTable(12 frames, layout=\"h w\", dtype=\"uint16\", compressed=0)"

    cells = Rumi.frames(image, "b (row h) (col w) -> row col (b h w)"; tile_size=2)
    @test cells.layout == "b h w"
    @test length(cells) == 6
    @test cells.data[1] == image[1:2, 1:2, :]
    @test size(cells.data[6]) == (1, 1, 2)

    interleaved = Rumi.frames(image, "b (row h) (col w) -> row col (h w b)"; tile_size=2)
    @test interleaved.data[1] == permutedims(image[1:2, 1:2, :], (3, 1, 2))

    # A single band is omitted from the layout, and a view cuts like its copy.
    single = Rumi.frames(view(image, :, :, 2:2), LAYOUT; tile_size=4)
    @test single.layout == "h w"
    @test single.data[1] == image[1:4, 1:3, 2]
    @test Rumi.frames(trues(2, 2, 1), LAYOUT; tile_size=2).data[1] isa Matrix{Bool}

    cube = reshape(Float32.(1:4 * 4 * 2 * 3), 4, 4, 2, 3)
    steps = Rumi.frames(cube, "t b (row h) (col w) -> row col t b (h w)"; tile_size=4)
    @test steps.layout == "h w"
    @test length(steps) == 6
    @test steps.data[1] == cube[:, :, 1, 1]
    @test steps.data[2] == cube[:, :, 2, 1]
    @test steps.data[3] == cube[:, :, 1, 2]
end

@testset "write" begin
    path = tempname() * ".rumi"
    table = Rumi.frames(image, LAYOUT; tile_size=2)
    table.compressed .= Ref(UInt8[1, 2, 3, 4])
    header = Rumi.write(path, table; bands=["B04", "B08"], time=["2025-08-08"],
                        transform=(10, 0, 739760, 0, -10, 5087560), crs="EPSG:32631")
    @test header isa Vector{UInt8}
    @test filesize(path) > 12 * 4

    file = Rumi.open(path, header)
    metadata = Rumi.info(file)
    @test metadata.shape == (2, 3, 5)
    @test metadata.dtype == "uint16"
    @test metadata.tile == (2, 2)
    @test metadata.frame_layout == "h w"
    @test metadata.frames == 12
    @test metadata.bands == ["B04", "B08"]
    @test metadata.crs == 32631
    @test metadata.transform == (10.0, 0.0, 739760.0, 0.0, -10.0, 5087560.0)
    @test metadata.time_kind == :instant
    @test metadata.time == [DateTime(2025, 8, 8)]
    @test metadata.pixel_is_point == false
    close(file)

    cube = zeros(UInt8, 4, 4, 1, 2)
    steps = Rumi.frames(cube, "t b (row h) (col w) -> row col t b (h w)"; tile_size=4)
    steps.compressed .= Ref(UInt8[1])
    header = Rumi.write(path, steps; bands=["red"],
                        time=[Date(2025, 8, 8), DateTime(2025, 8, 13, 10, 30)])
    file = Rumi.open(path, header)
    @test Rumi.info(file).time == [DateTime(2025, 8, 8), DateTime(2025, 8, 13, 10, 30)]
    @test Rumi.info(file).transform === nothing
    close(file)

    intervals = [("2025-08-01", "2025-08-08"),
                 ("2025-08-08T00:00:00", "2025-08-15T14:00:00+02:00")]
    header = Rumi.write(path, steps; bands=["red"], time=intervals,
                        transform=[10, 0, 0, 0, -10, 0, 0, 0, 1], crs=32631,
                        pixel_is_point=true)
    file = Rumi.open(path, header)
    metadata = Rumi.info(file)
    @test metadata.time_kind == :interval
    @test metadata.time[2] == (DateTime(2025, 8, 8), DateTime(2025, 8, 15, 12))
    @test metadata.pixel_is_point == true
    close(file)
    rm(path)
end

@testset "frames and write refuse bad arguments" begin
    @test_throws "names 3 axes" Rumi.frames(UInt16.(1:10), LAYOUT)
    @test_throws "names 2 axes" Rumi.frames(image, "(row h) (col w) -> row col (h w)")
    @test_throws "tile_size" Rumi.frames(image, LAYOUT; tile_size=0)
    @test_throws "does not store" Rumi.frames(fill('a', 2, 2, 1), LAYOUT)
    @test_throws ArgumentError Rumi.frames(image, "b (row h) (col w) -> row col q (h w)")
    @test_throws "tile_size must be at least 1" Rumi.frames(image, LAYOUT; tile_size=0)
    @test_throws "bands and time steps must be positive" Rumi.frames(zeros(UInt16, 5, 3, 0), LAYOUT)

    path = tempname() * ".rumi"
    table = Rumi.frames(image, LAYOUT; tile_size=2)
    attempt(table, time) = Rumi.write(path, table; bands=["B04", "B08"], time)
    @test_throws "frame 1 has no compressed bytes" attempt(table, ["2025-08-08"])
    table.compressed[1:11] .= Ref(UInt8[1])
    @test_throws "frame 12 has no compressed bytes" attempt(table, ["2025-08-08"])
    table.compressed[12] = UInt8[]
    @test_throws "the 12th frame has an empty payload" attempt(table, ["2025-08-08"])
    table.compressed[12] = UInt8[1]

    @test_throws "given together" Rumi.write(path, table; bands=["B04", "B08"],
                                             time=["2025-08-08"], crs=32631)
    @test_throws "EPSG code" Rumi.write(path, table; bands=["B04", "B08"],
                                        time=["2025-08-08"],
                                        transform=(10, 0, 0, 0, -10, 0), crs="UTM")
    @test_throws "six coefficients" Rumi.write(path, table; bands=["B04", "B08"],
                                               time=["2025-08-08"],
                                               transform=(10, 0, 0), crs=32631)
    @test_throws "one text per band" Rumi.write(path, table; bands=["B04"],
                                                time=["2025-08-08"])
    @test_throws "same text" Rumi.write(path, table; bands=["B04", "B04"],
                                        time=["2025-08-08"])
    @test_throws "NUL" Rumi.write(path, table; bands=["B04", "B\0"], time=["2025-08-08"])
    @test_throws "wrap a single" Rumi.write(path, table; bands="B04", time=["2025-08-08"])

    @test_throws "one entry per time step" attempt(table, ["2025-08-08", "2025-08-09"])
    @test_throws "wrap a single" attempt(table, Date(2025, 8, 8))
    @test_throws "wrap it as" attempt(table, (Date(2025, 8, 1), Date(2025, 8, 8)))
    @test_throws "not a mix" attempt(table, ["2025-08-08", ("2025-08-01", "2025-08-08")])
    @test_throws "not an ISO date" attempt(table, ["8 August"])
    @test_throws "not an ISO date" attempt(table, ["2025-02-30"])
    @test_throws "fraction of a second" attempt(table, ["2025-08-08T10:00:00.5"])
    @test_throws "invalid UTC offset" attempt(table, ["2025-08-08T10:00:00+25:00"])
    @test_throws "fraction of a second" attempt(table, [DateTime(2025, 8, 8, 10, 0, 0, 500)])
    @test_throws "a Date, a DateTime" attempt(table, [20250808])
    @test !isfile(path)
end

# Match the shared fixtures in Julia's (x, y, band) order.
counts = UInt16.([200 + b * 977 + y * 31 + x * 17 + (x * 7919 + y * 104729 + b * 31) % 97
                  for x in 0:39, y in 0:47, b in 0:2])
reflectance = Float32.([((y * 131 + x * 71) % 1000) / 4000 + 0.1 for x in 0:39, y in 0:47])
holes = copy(reflectance)
holes[[(y * 40 + x) % 97 == 0 for x in 0:39, y in 0:47]] .= NaN32
radiance = Float32.([50 + 4000 * (x + y) / 254 + (x * 7919 + y * 104729) % 61
                     for x in 0:127, y in 0:127])
# Exercises float16 ties, subnormals and NaNs.
half = Float16.([y == 0 ? 2048.0 + x % 8 : y == 1 ? (x - 20) / 3 * 2.0^-14 :
                 ((y * 131 + x * 71) % 1000) / 7 - 60 for x in 0:39, y in 0:47])
half[[(y * 40 + x) % 89 == 0 for x in 0:39, y in 0:47]] .= NaN16
# int64 samples below 2^53, with the most negative int64 as the sentinel.
wide = [(x * 7919 + y * 104729) % 1000 * 10^12 - 5 * 10^14 for x in 0:39, y in 0:47]
wide[[(y * 40 + x) % 83 == 0 for x in 0:39, y in 0:47]] .= typemin(Int64)

@testset "GeoZL" begin
    cases = [
        "lossless" => (counts, "planar>zigzag>zstd", (;)),
        "linear" => (counts, "planar>zigzag>pfor", (; error=3)),
        "linear-float" => (reflectance, "planar>zigzag>zstd", (; error=0.01)),
        "log" => (reflectance, "planar>zigzag>zstd", (; error="1%")),
        "sqrt" => (radiance, "planar>zigzag>zstd", (; error="SQRT:MAX_ERROR=0.5N,A=25,B=2")),
        "sentinel" => (counts, "planar>zigzag>zstd", (; nodata=200)),
        "nan" => (holes, "planar>zigzag>zstd", (;)),
        "half" => (half, "planar>zigzag>zstd", (;)),
        "int64" => (wide, "planar>zigzag>zstd", (; nodata=typemin(Int64))),
    ]
    for (name, (raster, method, options)) in cases
        expected = Base.read(joinpath(@__DIR__, "fixtures", "geozl-$name.frame"))
        graph = Rumi.GeoZL.graph(raster, method; options...)
        @test Rumi.GeoZL.compress(raster, graph) == expected
    end

    path = tempname() * ".rumi"
    table = Rumi.frames(counts, "b (row h) (col w) -> row col (b h w)"; tile_size=16)
    for (i, data) in enumerate(table.data)
        table.compressed[i] = Rumi.GeoZL.compress(data, Rumi.GeoZL.graph(data, "planar>zigzag>pfor"))
    end
    header = Rumi.write(path, table; bands=["B02", "B03", "B04"], time=["2026-01-01"])
    file = Rumi.open(path, header)
    @test Rumi.read(file) == counts
    close(file)

    bounded = Rumi.frames(reshape(reflectance, 40, 48, 1), LAYOUT; tile_size=16)
    for (i, data) in enumerate(bounded.data)
        graph = Rumi.GeoZL.graph(data, "planar>zigzag>zstd"; error=0.01)
        bounded.compressed[i] = Rumi.GeoZL.compress(data, graph)
    end
    header = Rumi.write(path, bounded; bands=["B04"], time=["2026-01-01"])
    file = Rumi.open(path, header)
    @test maximum(abs.(Rumi.read(file)[:, :, 1] .- reflectance)) <= 0.01
    close(file)
    rm(path)

    graph = Rumi.GeoZL.graph(counts, "planar>zigzag>zstd")
    @test (graph.width, graph.planes, graph.eltype) == (40, 3, UInt16)
    @test sprint(show, graph) ==
          "Rumi.GeoZL.Graph(planar>zigzag>zstd, UInt16, row width 40, 3 planes, lossless)"
    @test Rumi.GeoZL.graph(UInt8.(1:12), "planar>zigzag>zstd"; width=4).planes == 1
    zero = Rumi.GeoZL.graph(counts, "planar>zigzag>zstd"; error=0)
    @test zero.error === nothing
    @test endswith(sprint(show, zero), "lossless)")
    @test Rumi.GeoZL.graph(reflectance, "planar>zigzag>zstd"; error=0.01).error ==
          "LINEAR:MAX_ERROR=0.01"
    @test Rumi.GeoZL.graph(reflectance, "planar>zigzag>zstd"; error="1%").error ==
          "LOG:MAX_ERROR=1%"

    # missing takes the sentinel, or NaN in a float array, and compresses as it.
    floats = Rumi.GeoZL.graph([1.0 missing; 3.0 4.0], "planar>zigzag>zstd")
    @test Rumi.GeoZL.compress([1.0 missing; 3.0 4.0], floats) ==
          Rumi.GeoZL.compress([1.0 NaN; 3.0 4.0], floats)
    ints = Rumi.GeoZL.graph(Union{Missing,Int16}[1 missing; 3 4], "planar>zigzag>zstd";
                            nodata=-1)
    @test Rumi.GeoZL.compress(Union{Missing,Int16}[1 missing; 3 4], ints) ==
          Rumi.GeoZL.compress(Int16[1 -1; 3 4], ints)

    @test_throws "the tile dtype is not the graph's" Rumi.GeoZL.compress(Int16.(counts), graph)
    @test_throws "error must not be negative" Rumi.GeoZL.graph(counts, "planar>zigzag>zstd"; error=-1)
    @test_throws "between 0% and 100%" Rumi.GeoZL.graph(counts, "planar>zigzag>zstd"; error="100%")
    @test_throws "whole rows" Rumi.GeoZL.graph(counts, "planar>zigzag>zstd"; planes=7)
    @test_throws "no nodata sentinel" Rumi.GeoZL.graph(Float16.(counts), "planar>zigzag>zstd"; nodata=0)
    @test_throws "nodata needs" Rumi.GeoZL.graph(trues(4, 4), "planar>zigzag>zstd"; nodata=1)
    @test_throws "1, 2, 4 or 8 bytes" Rumi.GeoZL.graph(ComplexF64.(counts), "planar>zigzag>zstd")
    @test_throws "could not build the graph" Rumi.GeoZL.graph(counts, "no such graph")

    @test_throws "must not be a Bool" Rumi.GeoZL.graph(counts, "planar>zigzag>zstd"; error=true)
    @test_throws "NUL character" Rumi.GeoZL.graph(reflectance, "planar>zigzag>zstd"; error="1%\0x")
    for text in ("5", "", "0")
        @test_throws "a text `error` is a percentage" Rumi.GeoZL.graph(
            counts, "planar>zigzag>zstd"; error=text)
    end
    @test_throws "cannot use NaN" Rumi.GeoZL.graph(counts, "planar>zigzag>zstd"; nodata=NaN)
    @test_throws "does not fit" Rumi.GeoZL.graph(counts, "planar>zigzag>zstd"; nodata=-1)
    @test_throws "does not fit" Rumi.GeoZL.graph(counts, "planar>zigzag>zstd"; nodata=65536)
    @test_throws "does not fit" Rumi.GeoZL.graph(Int16.(counts), "planar>zigzag>zstd"; nodata=0.5)
    @test_throws "does not fit" Rumi.GeoZL.graph(reflectance, "planar>zigzag>zstd"; nodata=1e40)
    @test_throws "needs `nodata`" Rumi.GeoZL.graph([1, missing], "planar>zigzag>zstd"; width=2)
    @test_throws "Bool raster cannot hold" Rumi.GeoZL.graph([true, missing], "planar>zigzag>zstd";
                                                             width=2)
    @test_throws "give `width`" Rumi.GeoZL.graph(counts[:, 1, 1], "planar>zigzag>zstd")
    @test_throws "does not store" Rumi.GeoZL.graph(fill('a', 2, 2), "planar>zigzag>zstd")
end
