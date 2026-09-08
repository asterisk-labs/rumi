"""Python time-axis conversion and trailer round trips."""

import datetime as dt
import os
import struct

import numpy as np
import pytest
import rumi

geozl = pytest.importorskip("geozl")

PATTERN = "b (row h) (col w) -> row col (b h w)"
UTC = dt.UTC


def write(tmp_path, name, time=None):
    data = np.arange(2 * 32 * 32, dtype=np.uint16).reshape(2, 32, 32)
    tf = rumi.frames(data, PATTERN, 16)
    for f in tf:
        f.compressed = geozl.compress(
            f.data, graph=geozl.graph(f.data, "planar>zigzag>zstd"))
    path, _header = rumi.write(tmp_path / f"{name}.rumi", tf, time=time)
    return str(path)


def trailer(path):
    with open(path, "rb") as fh:
        return fh.read()[-28:]


def scale_of(path):
    return struct.unpack_from("<I", trailer(path), 24)[0]


def test_a_file_with_no_time_says_so_rather_than_omitting_it(tmp_path):
    assert rumi.info(source=write(tmp_path, "plain")).time == []


def test_a_dated_scene_round_trips_as_a_date(tmp_path):
    path = write(tmp_path, "scene", ["2024-08-25"])
    assert rumi.info(source=path).time == [dt.date(2024, 8, 25)]


def test_a_date_before_the_epoch_round_trips(tmp_path):
    path = write(tmp_path, "before", ["1969-12-31"])
    assert scale_of(path) == 86400
    assert rumi.info(source=path).time == [dt.date(1969, 12, 31)]


def test_a_date_costs_nothing_over_a_file_with_none(tmp_path):
    """One coordinate requires no packed residuals."""
    assert os.path.getsize(write(tmp_path, "scene", ["2024-08-25"])) \
        == os.path.getsize(write(tmp_path, "plain"))


def test_a_time_of_day_makes_the_axis_seconds(tmp_path):
    path = write(tmp_path, "exact", ["2024-08-25T14:32:07Z"])
    assert rumi.info(source=path).time == [dt.datetime(2024, 8, 25, 14, 32, 7, tzinfo=UTC)]


def test_an_acquisition_window_is_a_pair(tmp_path):
    window = ("2024-08-25T14:30:00Z", "2024-08-25T14:35:00Z")
    path = write(tmp_path, "window", [window])
    got = rumi.info(source=path).time
    assert got == [(dt.datetime(2024, 8, 25, 14, 30, tzinfo=UTC),
                    dt.datetime(2024, 8, 25, 14, 35, tzinfo=UTC))]


def test_a_window_also_costs_nothing(tmp_path):
    """Two coordinates require no packed residuals."""
    window = ("2024-08-25T14:30:00Z", "2024-08-25T14:35:00Z")
    assert os.path.getsize(write(tmp_path, "window", [window])) \
        == os.path.getsize(write(tmp_path, "plain"))


@pytest.mark.parametrize("value", [
    dt.date(2024, 8, 25),
    dt.datetime(2024, 8, 25, tzinfo=UTC),
    "2024-08-25",
    np.datetime64("2024-08-25"),
])
def test_every_accepted_type_names_the_same_day(tmp_path, value):
    got = rumi.info(source=write(tmp_path, "any", [value])).time[0]
    assert (got.date() if isinstance(got, dt.datetime) else got) \
        == dt.date(2024, 8, 25)


def test_a_naive_datetime_is_utc(tmp_path):
    """Naive datetimes are interpreted as UTC."""
    naive = dt.datetime(2024, 8, 25, 14, 0, 0)
    aware = dt.datetime(2024, 8, 25, 14, 0, 0, tzinfo=UTC)
    assert rumi.info(source=write(tmp_path, "naive", [naive])).time \
        == rumi.info(source=write(tmp_path, "aware", [aware])).time


def test_a_zone_is_converted_not_dropped(tmp_path):
    lima = dt.timezone(dt.timedelta(hours=-5))
    path = write(tmp_path, "lima", [dt.datetime(2024, 8, 25, 9, 0, tzinfo=lima)])
    assert rumi.info(source=path).time == [dt.datetime(2024, 8, 25, 14, 0, tzinfo=UTC)]


def test_a_zone_conversion_may_cross_into_the_next_day(tmp_path):
    lima = dt.timezone(dt.timedelta(hours=-5))
    value = dt.datetime(2024, 8, 25, 23, 0, tzinfo=lima)
    path = write(tmp_path, "next-day", [value])
    assert rumi.info(source=path).time == [
        dt.datetime(2024, 8, 26, 4, 0, tzinfo=UTC)]


# Invalid time inputs

@pytest.mark.parametrize("time, because", [
    ([], "empty list"),
    (["2024-08-25", "2024-08-26"], "one entry per time step"),
    ([("2024-08-25", "2024-08-26"), ("2024-08-27", "2024-08-28")],
     "one entry per time step"),
    ([("2024-08-25",)], "a pair of a start and an end"),
    ([("2024-08-25", "2024-08-25")], "covers nothing"),
    ([("2024-08-26", "2024-08-25")], "covers nothing"),
    (["not a date"], "not an ISO date"),
    ([12345], "date, a datetime or an ISO string"),
])
def test_an_axis_that_cannot_be_recorded_is_refused(tmp_path, time, because):
    with pytest.raises((ValueError, TypeError), match=because):
        write(tmp_path, "bad", time)


@pytest.mark.parametrize("time, because", [
    ("2024-08-25", "wrap a single coordinate"),
    (dt.date(2024, 8, 25), "wrap a single coordinate"),
    (dt.datetime(2024, 8, 25, tzinfo=UTC), "wrap a single coordinate"),
    (("2024-08-25", "2024-08-26"), "cannot be the list of steps"),
])
def test_a_coordinate_outside_a_list_is_named_not_iterated(tmp_path, time,
                                                           because):
    """Reject scalar values before treating them as time-step sequences."""
    with pytest.raises(TypeError, match=because):
        write(tmp_path, "bare", time)


def test_a_numpy_array_of_dates_is_a_list(tmp_path):
    stamps = np.array(["2024-08-25"], dtype="datetime64[D]")
    assert rumi.info(source=write(tmp_path, "np", stamps)).time == [dt.date(2024, 8, 25)]


def test_time_coordinates_may_come_from_a_generator(tmp_path):
    stamps = (value for value in ["2024-08-25"])
    path = write(tmp_path, "generator", stamps)
    assert rumi.info(source=path).time == [dt.date(2024, 8, 25)]


def test_instants_and_intervals_cannot_be_mixed(tmp_path):
    """A time axis cannot mix instants and intervals."""
    with pytest.raises(ValueError, match="all pairs"):
        write(tmp_path, "mixed", ["2024-08-25", ("2024-08-26", "2024-08-27")])


def test_one_whole_day_has_one_encoding(tmp_path):
    """Equivalent midnight values select the same day-scale encoding."""
    ways = ["2024-08-25", dt.date(2024, 8, 25), dt.datetime(2024, 8, 25),
            np.datetime64("2024-08-25")]
    written = {trailer(write(tmp_path, f"day{i}", [v]))
               for i, v in enumerate(ways)}
    assert len(written) == 1
    assert struct.unpack_from("<I", written.pop(), 24)[0] == 86400


def test_a_time_of_day_moves_the_scale_to_seconds(tmp_path):
    assert scale_of(write(tmp_path, "clocked", ["2024-08-25T14:32:07Z"])) == 1


def test_the_axis_says_what_kind_it_is(tmp_path):
    """Decoded time kind comes from time_type, not Python value shape."""
    assert rumi.info(source=write(tmp_path, "none")).time_kind is None
    assert rumi.info(source=write(tmp_path, "one", ["2024-08-25"])).time_kind == "instant"
    span = write(tmp_path, "span", [("2024-08-25", "2024-08-26")])
    assert rumi.info(source=span).time_kind == "interval"
    # Steps remain directly iterable.
    assert len(rumi.info(source=span).time) == 1
    for start, end in rumi.info(source=span).time:
        assert start < end


@pytest.mark.parametrize("value", [
    dt.datetime(2024, 8, 25, 14, 32, 7, 900000, tzinfo=UTC),
    np.datetime64("2024-08-25T14:32:07.900"),
    "2024-08-25T14:32:07.9",
])
def test_a_fraction_of_a_second_is_refused(tmp_path, value):
    """Reject fractional seconds instead of rounding them."""
    with pytest.raises(ValueError, match="fraction of a second"):
        write(tmp_path, "frac", [value])


def test_a_whole_second_still_goes_through(tmp_path):
    path = write(tmp_path, "whole", ["2024-08-25T14:32:07Z"])
    assert rumi.info(source=path).time == [
        dt.datetime(2024, 8, 25, 14, 32, 7, tzinfo=UTC)]
