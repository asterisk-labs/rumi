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
    @test Set(names(Rumi)) == Set([:Rumi, :Dataset, :FrameTable, :Metadata, :RumiError])
    @test sizeof(Rumi.RumiBFloat16) == 2
end

kind(f) = try
    f()
    nothing
catch err
    err isa RumiError ? err.kind : typeof(err)
end

# Runs before any read, while the process-wide settings may still change.
@testset "settings" begin
    @test Rumi.set_num_threads(2) === 2
    @test Rumi.get_num_threads() == 2
    verify = Rumi.get_checksum_verification()
    @test verify isa Bool
    @test Rumi.set_checksum_verification(verify) === verify
    @test_throws ArgumentError("num_threads must be in [1, 1024]") Rumi.set_num_threads(0)
    @test_throws ArgumentError("num_threads must be in [1, 1024]") Rumi.set_num_threads(2^40)
    @test_throws "must be an integer" Rumi.set_num_threads(true)
    @test_throws "must be an integer" Rumi.set_num_threads(2.0)
    @test_throws "must be a Bool" Rumi.set_checksum_verification(1)

    # A parallel read pins the pool size and the first decode the checksum setting.
    Rumi.read(path, Base.read(header_path))
    @test Rumi.get_num_threads() == 2
    @test Rumi.set_num_threads(2) == 2
    @test kind(() -> Rumi.set_num_threads(3)) === :state
    @test Rumi.set_checksum_verification(verify) === verify
    @test kind(() -> Rumi.set_checksum_verification(!verify)) === :state
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
    @test file.source == path
    @test file.source isa String
    @test file._bytes === nothing
    @test file.header == header
    @test file.metadata === metadata
    @test metadata.shape == (2, 3, 6, 8)
    @test metadata.time_count == 2
    @test metadata.dtype == "uint16"
    @test metadata.bands == ["B02", "B03", "B04"]
    @test metadata.crs == 32618
    @test metadata.transform == (10.0, 0.0, 500000.0, 0.0, -10.0, 8000000.0)
    @test metadata.time_kind == :instant
    @test metadata.time == [Date(2026, 1, 1), Date(2026, 1, 6)]
    @test eltype(metadata.time) == Date

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

    # An integer, a vector and a range select alike.
    @test Rumi.read(file; bands=2, time=2:2) == whole[:, :, 2:2, 2:2]
    @test Rumi.read(file; bands=2:3) == Rumi.read(file; bands=[2, 3]) == whole[:, :, 2:3, :]
    @test Rumi.read(file; time=2:-1:1) == whole[:, :, :, [2, 1]]
    @test Rumi.read(file; window=[2, 3, 3, 4]) == Rumi.read(file; window=(2, 3, 3, 4))
    @test_throws ArgumentError("bands takes a list or a range, not a tuple") Rumi.read(
        file; bands=(1, 3))
    @test_throws ArgumentError("time takes a list or a range, not a tuple") Rumi.read(
        file; time=(1,))
    @test_throws ArgumentError("every bands index must be an integer") Rumi.read(file; bands=true)
    @test_throws ArgumentError("every time index must be an integer") Rumi.read(
        file; time=[true, false])
    @test_throws "takes an integer, a list or a range" Rumi.read(file; bands=Set([1]))
    @test_throws ArgumentError("bands selects the 4th band, but this file has 3") Rumi.read(
        file; bands=1:10^9)
    @test_throws "window must be" Rumi.read(file; window=(1, 1, 2))
    @test_throws "window must be" Rumi.read(file; window=[1 1 2 2])

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
    @test kind(() -> Rumi.open(path, bad_header)) === :parse
    @test kind(() -> Rumi.open(path * ".missing", header)) === :io
    @test kind(() -> Rumi.info(Base.read(path)[1:200])) === :format
    @test_throws "header must be an AbstractVector{UInt8}" Rumi.open(path, "header")
    @test_throws "source must be a path, a URI or the file's bytes" Rumi.open(1, header)
    @test_throws "header is already carried" Rumi.read(file, header)

    batch = Rumi.read_many(
        [file, file];
        windows=[(1, 1, 2, 3), (3, 4, 2, 3)],
        bands=[2],
    )
    @test size(batch) == (3, 2, 1, 2, 2)
    @test batch[:, :, :, :, 1] == whole[1:3, 1:2, 2:2, :]
    @test batch[:, :, :, :, 2] == whole[4:6, 3:4, 2:2, :]
    @test Rumi.read_many([file, file]; windows=[1 1 2 3; 3 4 2 3], bands=[2]) == batch
    @test Rumi.read_many([file, file]; windows=[nothing, (1, 1, 6, 8)]) == cat(whole, whole; dims=5)
    @test Rumi.read_many([file]) == reshape(whole, size(whole)..., 1)
    @test_throws "window per row" Rumi.read_many([file]; windows=[1 1 2])
    @test_throws "one entry per Dataset" Rumi.read_many([file]; windows=[1 1 2 3; 1 1 2 3])
    @test_throws "a vector with one window per Dataset" Rumi.read_many(
        [file]; windows=((1, 1, 2, 3),))
    @test_throws "a list or a range, not a tuple" Rumi.read_many([file]; bands=(1, 2))
    @test_throws "vector of Datasets" Rumi.read_many([path])
    @test_throws "already carried by these Datasets" Rumi.read_many([file], [header])
    @test_throws "use read for one" Rumi.read_many(Base.read(path), [header])
    @test_throws "one header per source" Rumi.read_many([path], header)

    floating = Rumi.open(float_path, Base.read(float_header_path))
    float_chip = Rumi.read(floating; time=[2], bands=[3], window=(2, 3, 3, 4))
    @test eltype(float_chip) == Float64
    @test size(float_chip) == (4, 3, 1, 1)
    @test float_chip[1, 1, 1, 1] == 1210.2
    close(floating)

    close(file)
    @test !isopen(file)
    @test_throws ArgumentError("this Rumi Dataset is closed") Rumi.read(file)
    @test Rumi.info(file) === metadata

    opened = Rumi.open(path, header) do dataset
        @test isopen(dataset)
        dataset
    end
    @test !isopen(opened)
    @test Rumi.open(dataset -> size(Rumi.read(dataset)), path, header) == (8, 6, 3, 2)
end

@testset "bytes sources" begin
    header = Base.read(header_path)
    whole = Rumi.read(path, header)
    bytes = Base.read(path)
    file = Rumi.open(bytes, header)
    @test file.source isa AbstractVector{UInt8}
    @test file.source == bytes
    @test file.source !== bytes
    # The Dataset reads its own copy.
    fill!(bytes, 0x00)
    @test Rumi.read(file) == whole

    # Nothing reached through the Dataset can resize, rewrite or drop that copy.
    source = file.source
    @test_throws MethodError resize!(source, 0)
    @test_throws MethodError empty!(source)
    @test_throws MethodError push!(source, 0x00)
    @test_throws Exception source[1] = 0x00
    @test_throws ErrorException file.source = codeunits("other")
    @test_throws ErrorException file._bytes = nothing
    copied = collect(file.source)
    resize!(copied, 0)
    source = nothing
    GC.gc()
    @test Rumi.read(file) == whole
    @test file.source == Base.read(path)
    close(file)

    bytes = Base.read(path)
    @test Rumi.read(bytes, header; bands=[3, 1]) == whole[:, :, [3, 1], :]
    @test Rumi.read(view(bytes, :), header) == whole
    @test Rumi.read_many([bytes, path], [header, header]) == cat(whole, whole; dims=5)
    @test Rumi.info(bytes).header == header
    @test Rumi.open(dataset -> Rumi.read(dataset; time=1), bytes, header) == whole[:, :, :, 1:1]

    # A one-shot read borrows the caller's bytes only for the call, so resizing
    # them afterwards breaks nothing.
    loose = Base.read(path)
    @test Rumi.read(loose, header) == whole
    @test Rumi.read_many([loose], [header]) == reshape(whole, size(whole)..., 1)
    @test Rumi.info(loose).header == header
    @test length(loose) == filesize(path)
    empty!(loose)
    GC.gc()
    @test Rumi.read(Base.read(path), header) == whole
    @test kind(() -> Rumi.read(loose, header)) === :format
end

@testset "one-shot reads" begin
    header = Base.read(header_path)
    bytes = Base.read(path)
    whole = Rumi.open(Rumi.read, path, header)
    @test Rumi.read(path, header) == whole
    @test Rumi.read(bytes, header; bands=2:3, window=(2, 3, 3, 4)) == whole[3:6, 2:4, 2:3, :]
    @test Rumi.read_many([path, bytes], [header, header]; windows=[1 1 2 3; 3 4 2 3]) ==
          cat(whole[1:3, 1:2, :, :], whole[4:6, 3:4, :, :]; dims=5)

    # Without rumi_info the trailer is never fetched, so a file cut inside it still reads.
    cut = bytes[1:end-10]
    @test kind(() -> Rumi.open(cut, header)) === :format
    @test Rumi.read(cut, header) == whole
    @test Rumi.read_many([cut], [header]) == reshape(whole, size(whole)..., 1)

    bad = copy(header)
    bad[1] = xor(bad[1], 0x01)
    @test kind(() -> Rumi.read(path, bad)) === :parse
    @test kind(() -> Rumi.read(bytes, bad)) === :parse
    @test kind(() -> Rumi.read_many([path, bytes], [header, bad])) === :parse
    @test kind(() -> Rumi.read(path * ".missing", header)) === :io
    @test kind(() -> Rumi.read_many([bytes, path * ".missing"], [header, header])) === :io
    @test_throws "header must be" Rumi.read(path, "header")
    @test_throws "needs at least one item" Rumi.read_many(String[], Vector{UInt8}[])
    @test_throws "a list or a range, not a tuple" Rumi.read(bytes, header; time=(1, 2))
    # Failed reads free their handles and leave later reads intact.
    @test Rumi.read(bytes, header) == whole
end

# Another writer may store a time no binding's date type holds.
@testset "time outside the years 1 to 9999 on read" begin
    path = tempname() * ".rumi"
    fields(kind, epoch, step) =
        [kind; 0x00; reinterpret(UInt8, Int64[epoch, step]); reinterpret(UInt8, UInt32[86400])]
    days(date) = Dates.value(date - Date(1970))
    function packed(x, pattern)
        table = Rumi.frames(x, pattern, 4)
        for (i, data) in enumerate(table.data)
            table.compressed[i] = Rumi.GeoZL.compress(data, Rumi.GeoZL.graph(data, "planar>zigzag>zstd"))
        end
        return table
    end
    cube = packed(zeros(UInt8, 4, 4, 1, 2), "t b (row h) (col w) -> row col t b (h w)")
    flat = packed(zeros(UInt8, 4, 4, 1), "b (row h) (col w) -> row col b (h w)")
    cases = [
        (cube, [Date(2025, 8, 8), Date(2025, 8, 9)], 0x02, 1, 3_000_000, "2nd"),
        (cube, [Date(2025, 8, 8), Date(2025, 8, 9)], 0x02, 1, 1, "1st"),
        (flat, [(Date(2025, 8, 1), Date(2025, 8, 8))], 0x01, 7, 3_000_000, "1st"),
    ]
    for (table, time, kind, step, patched, nth) in cases
        header = Rumi.write(path, table; bands=["red"], time)
        bytes = Base.read(path)
        start = days(time[1] isa Tuple ? time[1][1] : time[1])
        at = findfirst(fields(kind, start, step), bytes)
        @test at !== nothing
        epoch = nth == "1st" && kind == 0x02 ? -1_000_000 : start
        bytes[at] = fields(kind, epoch, patched)
        message = "the $nth time step falls outside the years 1 to 9999"
        @test_throws RumiError(:format, message) Rumi.info(bytes)
        @test_throws RumiError(:format, message) Rumi.open(bytes, header)
        # A one-shot read never fetches the trailer, so it still returns the samples.
        @test Rumi.read(bytes, header) == (table === cube ? zeros(UInt8, 4, 4, 1, 2) : zeros(UInt8, 4, 4, 1))
    end
    rm(path)
end

@testset "info" begin
    header = Base.read(header_path)
    from_path = Rumi.info(path)
    @test from_path.header == header
    @test from_path.bands == ["B02", "B03", "B04"]
    @test from_path.crs == 32618
    @test Rumi.info(path; header).time == from_path.time

    alone = Rumi.info(; header)
    @test alone.header == header
    @test alone.shape == from_path.shape
    @test alone.dtype == "uint16"
    @test alone.tile == from_path.tile
    @test alone.frame_layout == from_path.frame_layout
    @test alone.index_order == from_path.index_order
    @test alone.frames == from_path.frames
    for field in (:bands, :time, :time_kind, :transform, :crs, :pixel_is_point)
        @test getfield(alone, field) === nothing
    end

    other = copy(header)
    other[end] = xor(other[end], 0x01)
    @test_throws ArgumentError("external header does not match source") Rumi.info(
        path; header=other)
    @test_throws ArgumentError("info needs a source, a header, or both") Rumi.info()
    file = Rumi.open(path, header)
    @test Rumi.info(file) === file.metadata
    @test_throws ArgumentError("header is already carried by this Dataset") Rumi.info(
        file; header)
    close(file)
    @test_throws "header must be" Rumi.info(; header="bytes")
end

@testset "errors" begin
    @test RumiError(:io, "gone").kind === :io
    @test RumiError("broken") == RumiError(:internal, "broken")
    @test sprint(showerror, RumiError(:decode, "bad frame")) == "RumiError(:decode): bad frame"
    @test_throws "is not a Rumi error kind" RumiError(:invalid, "no")
end

# An Image of 5 columns, 3 rows and 2 bands, as Julia holds it, (x, y, band).
image = reshape(UInt16.(0:29), 5, 3, 2)
const LAYOUT = "b (row h) (col w) -> row col b (h w)"

@testset "frames" begin
    table = Rumi.frames(image, LAYOUT; tile_size=2)
    @test table isa FrameTable{UInt16,2}
    @test table.layout == "h w"
    @test table.dtype == "uint16"
    @test table.pattern == LAYOUT
    @test length(table) == 12
    @test (table.tiles_across, table.tiles_down) == (3, 2)
    @test table.row == [1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2]
    @test table.col == [1, 1, 2, 2, 3, 3, 1, 1, 2, 2, 3, 3]
    @test table.band == repeat([1, 2], 6)
    @test table.time === nothing
    @test Rumi.frames(image, LAYOUT, 2).data == table.data
    @test_throws "tile_size must be an integer" Rumi.frames(image, LAYOUT, 2.0)
    @test_throws "tile_size must be an integer" Rumi.frames(image, LAYOUT; tile_size=true)
    @test all(isnothing, table.compressed)
    @test table.data[1] == image[1:2, 1:2, 1]
    @test table.data[2] == image[1:2, 1:2, 2]
    @test table.data[5] == reshape(image[5, 1:2, 1], 1, 2)
    @test table.data[12] == reshape(image[5:5, 3:3, 2], 1, 1)
    @test sprint(show, table) ==
          "Rumi.FrameTable(12 frames, layout=\"h w\", dtype=\"uint16\", compressed=0)"

    cells = Rumi.frames(image, " b (row h) (col w) -> row col (b h w) "; tile_size=2)
    @test cells.layout == "b h w"
    @test cells.pattern == "b (row h) (col w) -> row col (b h w)"
    @test length(cells) == 6
    @test (cells.row, cells.col) == ([1, 1, 1, 2, 2, 2], [1, 2, 3, 1, 2, 3])
    @test cells.band === nothing
    @test cells.time === nothing
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
    @test steps.band == [1, 2, 1, 2, 1, 2]
    @test steps.time == [1, 1, 2, 2, 3, 3]
    @test steps.row == ones(Int, 6)
end

@testset "frame_start" begin
    start = Rumi.frame_start(size(image), LAYOUT, 2)
    @test start == Rumi.frame_start(size(image), LAYOUT; tile_size=2)
    @test start == Rumi.frame_start(collect(size(image)), LAYOUT, 2)

    # The offset is where write puts the first payload.
    path = tempname() * ".rumi"
    table = Rumi.frames(image, LAYOUT, 2)
    for i in 1:length(table)
        table.compressed[i] = UInt8[0xa0 + i, 0xb0, 0xc0, 0xd0]
    end
    header = Rumi.write(path, table; bands=["B04", "B08"], time=["2025-08-08"])
    @test first(findfirst(UInt8[0xa1, 0xb0, 0xc0, 0xd0], Base.read(path))) == start + 1
    file = Rumi.open(path, header)
    @test Rumi._header(file).base_frame_offset == start
    close(file)
    rm(path)

    cube = (4, 4, 2, 3)
    pattern = "t b (row h) (col w) -> row col t b (h w)"
    @test Rumi.frame_start(cube, pattern; tile_size=4) > Rumi.frame_start(cube[1:3], LAYOUT, 4)
    @test Rumi.frame_start((1024, 1024, 4), LAYOUT) > 0
    @test_throws "names 3 axes" Rumi.frame_start((5, 3), LAYOUT, 2)
    @test_throws "tuple or vector of integers" Rumi.frame_start((5.0, 3, 2), LAYOUT, 2)
    @test_throws "negative" Rumi.frame_start((5, -3, 2), LAYOUT, 2)
    @test_throws "tile_size must be at least 1" Rumi.frame_start((5, 3, 2), LAYOUT, 0)
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
    @test metadata.time == [Date(2025, 8, 8)]
    @test eltype(metadata.time) == Date
    @test metadata.pixel_is_point == false
    # The placeholder payloads are not GeoZL frames.
    @test kind(() -> Rumi.read(file)) === :decode
    close(file)

    header = Rumi.write(path, table; bands=("B04", "B08"), time=[Date(2025, 8, 8)])
    @test Rumi.info(path; header).bands == ["B04", "B08"]

    cube = zeros(UInt8, 4, 4, 1, 2)
    steps = Rumi.frames(cube, "t b (row h) (col w) -> row col t b (h w)"; tile_size=4)
    steps.compressed .= Ref(UInt8[1])
    header = Rumi.write(path, steps; bands=["red"],
                        time=[Date(2025, 8, 8), DateTime(2025, 8, 13, 10, 30)])
    file = Rumi.open(path, header)
    @test Rumi.info(file).time == [DateTime(2025, 8, 8), DateTime(2025, 8, 13, 10, 30)]
    @test eltype(Rumi.info(file).time) == DateTime
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
    @test metadata.time[1] === (DateTime(2025, 8, 1), DateTime(2025, 8, 8))
    @test metadata.time[2] == (DateTime(2025, 8, 8), DateTime(2025, 8, 15, 12))
    @test metadata.pixel_is_point == true
    close(file)

    # Every binding's date type holds the years 1 to 9999, so the writer keeps to them.
    edges = [Date(1, 1, 1), DateTime(9999, 12, 31, 23, 59, 59)]
    header = Rumi.write(path, steps; bands=["red"], time=edges)
    @test Rumi.info(path; header).time == edges
    @test_throws ArgumentError("the 2nd time step falls outside the years 1 to 9999") Rumi.write(
        path, steps; bands=["red"], time=[Date(2025, 8, 8), Date(10000, 1, 1)])
    @test_throws ArgumentError("the 1st time step falls outside the years 1 to 9999") Rumi.write(
        path, steps; bands=["red"], time=[DateTime(0, 12, 31, 23), Date(2025, 8, 8)])
    @test_throws ArgumentError("the 2nd time step falls outside the years 1 to 9999") Rumi.write(
        path, steps; bands=["red"],
        time=[(Date(2025, 8, 1), Date(2025, 8, 8)), (Date(9999, 12, 31), Date(10000, 1, 1))])
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
    @test_throws "not an ISO date" attempt(table, ["2025-08-08T24:00"])
    @test_throws "not an ISO date" attempt(table, ["2025-08-08T24:00:00Z"])
    @test_throws ArgumentError("pixel_is_point must be true or false, got 1") Rumi.write(
        path, table; bands=["B04", "B08"], time=["2025-08-08"], pixel_is_point=1)
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
    @test graph isa Rumi.GeoZL.Graph{UInt16}
    @test eltype(graph) === UInt16
    @test (graph.method, graph.width, graph.planes, graph.dtype, graph.itemsize,
           graph.error, graph.nodata) == ("planar>zigzag>zstd", 40, 3, "uint16", 2, nothing, nothing)
    @test sprint(show, graph) ==
          "Rumi.GeoZL.Graph(planar>zigzag>zstd, uint16, row width 40, 3 planes, lossless)"
    floats = Rumi.GeoZL.graph(reflectance, "planar>zigzag>zstd"; error=0.01, planes=2)
    @test (floats.dtype, floats.itemsize, floats.planes) == ("float32", 4, 2)
    @test endswith(sprint(show, floats), "float32, row width 40, 2 planes, LINEAR:MAX_ERROR=0.01)")
    @test Rumi.GeoZL.graph(trues(4, 4), "id>zstd").dtype == "bool"
    @test Rumi.GeoZL.compress(counts; graph) == Rumi.GeoZL.compress(counts, graph)
    @test Rumi.GeoZL.graph(UInt8.(1:12), "planar>zigzag>zstd"; width=4).planes == 1
    zero = Rumi.GeoZL.graph(counts, "planar>zigzag>zstd"; error=0)
    @test zero.error === nothing
    @test endswith(sprint(show, zero), "lossless)")
    @test Rumi.GeoZL.graph(reflectance, "planar>zigzag>zstd"; error=0.01).error ==
          "LINEAR:MAX_ERROR=0.01"
    @test Rumi.GeoZL.graph(reflectance, "planar>zigzag>zstd"; error="1%").error ==
          "LOG:MAX_ERROR=1%"

    # Floats are spelled as Python's repr spells a float64 and NumPy's str a
    # float32 or float16, so every binding records the same recipe.
    spellings = Pair{Any,String}[
        1e-5 => "1e-05", 0.01 => "0.01", 2.0 => "2.0", 1e20 => "1e+20", 1e16 => "1e+16",
        1e15 => "1000000000000000.0", 123456.789 => "123456.789", 0.0001 => "0.0001",
        0.00012 => "0.00012", 1.5e-7 => "1.5e-07", 9999999999999998.0 => "9999999999999998.0",
        1 // 3 => "0.3333333333333333", π => "3.141592653589793", 5e-324 => "5e-324",
        floatmax(Float64) => "1.7976931348623157e+308", 0.1f0 => "0.1", 1f7 => "1e+07",
        1.5f-5 => "1.5e-05", 999999f0 => "999999.0", Float16(0.5) => "0.5",
        Float16(1000) => "1e+03", Float16(0.0001) => "0.0001", 3 => "3", UInt8(2) => "2",
        -1e-5 => "-1e-05", 0.0 => "0.0",
    ]
    for (value, text) in spellings
        @test Rumi.GeoZL._error_text(value) == text
    end
    ramp = reshape(Float64.(1:64), 8, 8)
    for (value, text) in Pair{Any,String}[1e-5 => "1e-05", 1e20 => "1e+20", 2.0 => "2.0",
                                          1f7 => "1e+07"]
        @test Rumi.GeoZL.graph(ramp, "planar>zigzag>zstd"; error=value).error ==
              "LINEAR:MAX_ERROR=$text"
    end
    @test_throws ArgumentError("error must not be negative, got -1e-05") Rumi.GeoZL.graph(
        ramp, "planar>zigzag>zstd"; error=-1e-5)
    @test_throws "too small for a Float64" Rumi.GeoZL.graph(ramp, "planar>zigzag>zstd";
                                                            error=big"1e-400")

    # missing takes the sentinel, or NaN in a float array, and compresses as it.
    floats = Rumi.GeoZL.graph([1.0 missing; 3.0 4.0], "planar>zigzag>zstd")
    @test Rumi.GeoZL.compress([1.0 missing; 3.0 4.0], floats) ==
          Rumi.GeoZL.compress([1.0 NaN; 3.0 4.0], floats)
    ints = Rumi.GeoZL.graph(Union{Missing,Int16}[1 missing; 3 4], "planar>zigzag>zstd";
                            nodata=-1)
    @test Rumi.GeoZL.compress(Union{Missing,Int16}[1 missing; 3 4], ints) ==
          Rumi.GeoZL.compress(Int16[1 -1; 3 4], ints)

    @test_throws ArgumentError("this graph was built for dtype uint16, got int16") Rumi.GeoZL.compress(
        Int16.(counts), graph)
    @test_throws MethodError Rumi.GeoZL.compress(counts, "planar>zigzag>zstd")
    @test_throws "error must not be negative" Rumi.GeoZL.graph(counts, "planar>zigzag>zstd"; error=-1)
    @test_throws "between 0% and 100%" Rumi.GeoZL.graph(counts, "planar>zigzag>zstd"; error="100%")
    @test_throws "whole rows" Rumi.GeoZL.graph(counts, "planar>zigzag>zstd"; planes=7)
    @test_throws "a half float carries no sentinel" Rumi.GeoZL.graph(
        Float16.(counts), "planar>zigzag>zstd"; nodata=0)
    @test_throws "nodata needs" Rumi.GeoZL.graph(trues(4, 4), "planar>zigzag>zstd"; nodata=1)
    @test_throws ArgumentError("dtype complex128 is 16 bytes per element, an OpenZL numeric " *
                               "stream is 1, 2, 4 or 8") Rumi.GeoZL.graph(
        ComplexF64.(counts), "planar>zigzag>zstd")
    @test_throws ArgumentError("GeoZL graph failed (method = \"no such graph\"): unknown method " *
                               "\"no such graph\" (ZL error code 21)") Rumi.GeoZL.graph(
        counts, "no such graph")

    @test_throws "must not be a Bool" Rumi.GeoZL.graph(counts, "planar>zigzag>zstd"; error=true)
    @test_throws "NUL character" Rumi.GeoZL.graph(reflectance, "planar>zigzag>zstd"; error="1%\0x")
    for text in ("5", "", "0")
        @test_throws "error must be a number, a percentage such as" Rumi.GeoZL.graph(
            counts, "planar>zigzag>zstd"; error=text)
    end
    @test_throws "cannot use NaN" Rumi.GeoZL.graph(counts, "planar>zigzag>zstd"; nodata=NaN)
    @test_throws "does not fit" Rumi.GeoZL.graph(counts, "planar>zigzag>zstd"; nodata=-1)
    @test_throws "does not fit" Rumi.GeoZL.graph(counts, "planar>zigzag>zstd"; nodata=65536)
    @test_throws "does not fit" Rumi.GeoZL.graph(Int16.(counts), "planar>zigzag>zstd"; nodata=0.5)
    @test_throws "does not fit" Rumi.GeoZL.graph(reflectance, "planar>zigzag>zstd"; nodata=1e40)
    @test_throws "needs `nodata`" Rumi.GeoZL.graph([1, missing], "planar>zigzag>zstd"; width=2)
    @test_throws "a bool raster needs `nodata`" Rumi.GeoZL.graph(
        [true, missing], "planar>zigzag>zstd"; width=2)
    @test_throws "give `width`" Rumi.GeoZL.graph(counts[:, 1, 1], "planar>zigzag>zstd")
    @test_throws "does not store" Rumi.GeoZL.graph(fill('a', 2, 2), "planar>zigzag>zstd")
end

# The checks that need a Julia value are made in the binding, and these
# messages mirror GeoZL.jl's own tests with Rumi's dtype names. The rest come
# from librumi, whose texts follow the GeoZL packages.
@testset "GeoZL argument errors" begin
    m = "planar>zigzag>pfor"
    tile = counts[:, :, 1]
    message(f) = try
        f()
        ""
    catch err
        err isa ArgumentError ? err.msg : rethrow()
    end
    for bad in (3.7, 48.0, "48", true, 0, -1, 2^33, typemax(UInt32) + 1)
        @test_throws ArgumentError Rumi.GeoZL.graph(vec(tile), m; width=bad)
        @test_throws ArgumentError Rumi.GeoZL.graph(counts, m; planes=bad)
    end
    @test message(() -> Rumi.GeoZL.graph(vec(tile), m; width=3.7)) ==
          "`width` must be an integer from 1 to 4294967295, got 3.7"
    @test message(() -> Rumi.GeoZL.graph(counts, m; planes=0)) ==
          "`planes` must be an integer from 1 to 4294967295, got 0"
    @test Rumi.GeoZL.graph(vec(tile), m; width=UInt8(40)).width == 40
    @test Rumi.GeoZL.graph(vec(tile), m; width=big(40), planes=Int8(2)).planes == 2

    @test message(() -> Rumi.GeoZL.graph(tile, "id>zstd\0junk")) ==
          "`method` must not contain a NUL character"
    @test message(() -> Rumi.GeoZL.graph(tile, "")) ==
          "`method` must be a recipe such as \"planar>zigzag>pfor\""
    @test message(() -> Rumi.GeoZL.graph(reflectance, m; error="LINEAR:MAX_ERROR=2\0junk")) ==
          "`error` must not contain a NUL character"
    @test message(() -> Rumi.GeoZL.graph(tile, m; error=true)) == "`error` must not be a Bool"
    @test message(() -> Rumi.GeoZL.graph(tile, m; error=[1])) ==
          "`error` must be a number, percentage or recipe, got [1]"
    # librumi reads "5" as LINEAR and "" as lossless; GeoZL.jl refuses both as text.
    for (text, shown) in (("5", "\"5\""), ("", "\"\""), ("ten", "\"ten\""))
        @test message(() -> Rumi.GeoZL.graph(tile, m; error=text)) ==
              "error must be a number, a percentage such as \"10%\", or a LINEAR, LOG or " *
              "SQRT recipe, got $shown"
    end
    @test message(() -> Rumi.GeoZL.graph(tile, m; error=-1)) == "error must not be negative, got -1"
    @test message(() -> Rumi.GeoZL.graph(tile, m; error=Inf)) == "error must be finite, got Inf"
    @test message(() -> Rumi.GeoZL.graph(tile, m; error="ten%")) ==
          "relative error must look like \"10%\", got \"ten%\""
    @test message(() -> Rumi.GeoZL.graph(tile, m; error="150%")) ==
          "relative error must be between 0% and 100%, got \"150%\""
    @test message(() -> Rumi.GeoZL.graph(UInt16.(1:256), m; width=16, planes=3)) ==
          "3 planes do not split 256 elements into whole rows of 16"
    bfloat = reinterpret(Rumi.RumiBFloat16, UInt16.(tile))
    @test message(() -> Rumi.GeoZL.graph(bfloat, m; error=1)) ==
          "the quantizers do not support dtype bfloat16"
    @test message(() -> Rumi.GeoZL.graph(bfloat, m; nodata=0)) ==
          "nodata needs a dtype geozl knows, got bfloat16"

    for bad in (true, "5", missing, [1], 1 + 2im, π, Inf, 1.5, -1)
        @test_throws ArgumentError Rumi.GeoZL.graph(tile, m; nodata=bad)
    end
    @test message(() -> Rumi.GeoZL.graph(tile, m; nodata=NaN)) ==
          "a uint16 raster cannot use NaN as nodata"
    @test message(() -> Rumi.GeoZL.graph(tile, m; nodata=70000)) == "nodata 70000 does not fit uint16"
    @test message(() -> Rumi.GeoZL.graph(tile, m; nodata=true)) == "`nodata` must be a number, got true"
    @test message(() -> Rumi.GeoZL.graph(trues(4, 4), "id>zstd"; nodata=0)) ==
          "nodata needs a dtype geozl knows, got bool"
    @test message(() -> Rumi.GeoZL.graph([1, missing], m; width=2)) ==
          "`x` has missing values; a int64 raster needs `nodata` to mark them"
    @test message(() -> Rumi.GeoZL.compress(Int16.(tile), Rumi.GeoZL.graph(tile, m))) ==
          "this graph was built for dtype uint16, got int16"
end
