using Rumi
using Test

path = get(ENV, "RUMI_TEST_FILE", joinpath(@__DIR__, "fixtures", "binding.rumi"))
header_path = get(ENV, "RUMI_TEST_HEADER", joinpath(@__DIR__, "fixtures", "binding.header"))
float_path = joinpath(@__DIR__, "fixtures", "binding-f64.rumi")
float_header_path = joinpath(@__DIR__, "fixtures", "binding-f64.header")

@testset "package API" begin
    @test Rumi.open !== Base.open
    @test Rumi.read !== Base.read
    @test :open ∉ names(Rumi)
    @test :read ∉ names(Rumi)
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
    @test_throws ArgumentError Rumi.read(file; bands=[0])
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
