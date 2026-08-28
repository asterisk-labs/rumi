"""Python bindings for the core frame-pattern API."""

import pytest
from rumi._pattern import (
    PatternError,
    compile_layout_unit,
    compile_pattern,
    frame_at,
    frame_count,
    layout_name,
    unit_indexes_bands,
)

CELL = "b (row h) (col w) -> row col (b h w)"
TILE = "b (row h) (col w) -> row col b (h w)"
CHUNKY = "b (row h) (col w) -> row col (h w b)"


@pytest.mark.parametrize("text, frame, unit", [
    (CELL,   ("b", "h", "w"), 1),
    (TILE,   ("h", "w"),      0),
    (CHUNKY, ("h", "w", "b"), 2),
])
def test_role_codes_come_back_as_letters(text, frame, unit):
    p = compile_pattern(text)
    assert p.frame_axes == frame
    assert p.frame_unit(3, 1) == unit
    assert str(p) == " ".join(frame)


def test_the_input_order_crosses_intact():
    """A (Y, X, B) array needs no transpose before frames()."""
    assert compile_pattern("(row h) (col w) b -> row col (b h w)").input_axes \
        == ("y", "x", "b")
    assert compile_pattern(CELL).input_axes == ("b", "y", "x")


def test_the_columns_a_table_shows_follow_the_layout():
    assert compile_pattern(TILE).index_columns(3, 1) == ("row", "col", "band")
    assert compile_pattern(CELL).index_columns(3, 1) == ("row", "col")
    assert not compile_pattern(TILE).holds("b")
    assert compile_pattern(CHUNKY).holds("b")


def test_one_band_leaves_nothing_for_the_index_to_walk():
    """Singleton non-spatial axes are omitted from the recorded unit."""
    assert compile_pattern(TILE).index_columns(1, 1) == ("row", "col")
    for text in (TILE, CELL, CHUNKY):
        assert compile_pattern(text).frame_unit(1, 1) == 0


@pytest.mark.parametrize("unit, name", [(0, "h w"), (1, "b h w"), (2, "h w b")])
def test_a_unit_names_itself_both_ways(unit, name):
    assert layout_name(unit, 3, 1) == name
    assert compile_layout_unit(name, 3, 1) == unit


def test_the_single_axis_of_a_unit_follows_the_shape():
    """Single-axis units resolve to band or time from the raster extents."""
    assert layout_name(1, 3, 1) == "b h w"
    assert layout_name(1, 1, 3) == "t h w"
    assert layout_name(2, 3, 1) == "h w b"
    assert layout_name(2, 1, 3) == "h w t"


def test_unit_indexes_bands_matches_the_layout():
    assert unit_indexes_bands(0, 3, 1)
    assert not unit_indexes_bands(1, 3, 1)
    assert not unit_indexes_bands(2, 3, 1)


def test_geometry_crosses_intact():
    """130 x 100 on a 32 tile is 5 across by 4 down, with a ragged corner."""
    assert frame_count(2, 130, 100, 32, 3, 1) == (5, 4, 20)
    at = frame_at(2, 130, 100, 32, 3, 1, 19)
    assert (at.row, at.col, at.h, at.w) == (3, 4, 4, 2)
    assert at.dims == (4, 2, 3)
    assert frame_at(0, 130, 100, 32, 3, 1, 19).band == 1


# Preserve the core's pattern error in the Python exception.

@pytest.mark.parametrize("text, because", [
    ("b (row h) (col w) -> col row (b h w)", "grid axes lead"),
    ("b (row h) (col w) -> row col (b w h)", "adjacent"),
    ("b (row h) (col w) -> row col ghost (b h w)", "never introduced"),
    ("b (row h) (col w) row col (b h w)", "one '->'"),
])
def test_a_refusal_arrives_as_a_python_error(text, because):
    with pytest.raises(PatternError, match=because):
        compile_pattern(text)


def test_an_unknown_layout_is_refused():
    with pytest.raises(PatternError, match="names no layout"):
        compile_layout_unit("b w h", 3, 1)
    with pytest.raises(ValueError, match="unknown frame_unit"):
        layout_name(0xFF, 3, 1)


def test_a_pattern_is_a_string():
    with pytest.raises(PatternError, match="is a string, got"):
        compile_pattern(("b", "h", "w"))
    with pytest.raises(PatternError, match="is a string, got"):
        compile_layout_unit(1, 3, 1)
