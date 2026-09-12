"""Notebook representations communicate the physical frame layout."""

import numpy as np
import pytest
import rumi


def _cube(pattern):
    data = np.zeros((2, 2, 32, 32), dtype=np.uint16)
    return rumi.frames(data, pattern, tile_size=16)


def test_tile_frame_svg_shows_band_then_time_order():
    table = _cube("t b (row h) (col w) -> row col b t (h w)")
    body = table._repr_html_()

    assert 'data-frame-figure="0:h w"' in body
    assert "Band-first tiles" in body
    assert "slow → fast" not in body


def test_tile_frame_svg_shows_time_then_band_order():
    table = _cube("t b (row h) (col w) -> row col t b (h w)")
    body = table._repr_html_()

    assert 'data-frame-figure="9:h w"' in body
    assert "Time-first tiles" in body


@pytest.mark.parametrize(
    ("data", "pattern", "name", "description"),
    [
        (
            np.zeros((32, 32), dtype=np.uint16),
            "(row h) (col w) -> row col (h w)",
            "Spatial tiles",
            "per spatial tile",
        ),
        (
            np.zeros((3, 32, 32), dtype=np.uint16),
            "b (row h) (col w) -> row col b (h w)",
            "Band tiles",
            "Bands stay independently addressable",
        ),
        (
            np.zeros((3, 32, 32), dtype=np.uint16),
            "t (row h) (col w) -> row col t (h w)",
            "Time tiles",
            "Times stay independently addressable",
        ),
    ],
)
def test_singleton_tile_layout_names_the_axis_that_varies(data, pattern, name, description):
    body = rumi.frames(data, pattern, tile_size=16)._repr_html_()

    assert name in body
    assert description in body


def test_planar_cell_frame_svg_shows_axis_order():
    image = np.zeros((4, 1024, 1024), dtype=np.uint16)
    table = rumi.frames(
        image,
        "b (row h) (col w) -> row col (b h w)",
        tile_size=256,
    )
    body = table._repr_html_()

    assert table.layout == "b h w"
    assert "Band planes" in body
    assert "contiguous spatial planes" in body
    assert "rumi tile grid" not in body


def test_pixel_interleaved_cell_frame_svg_shows_axis_order():
    image = np.zeros((3, 32, 32), dtype=np.uint16)
    table = rumi.frames(
        image,
        "b (row h) (col w) -> row col (h w b)",
        tile_size=16,
    )
    body = table._repr_html_()

    assert "Pixel spectra" in body
    assert "complete spectrum" in body


def test_mixed_cell_frame_svg_names_both_storage_behaviors():
    table = _cube("t b (row h) (col w) -> row col (b h w t)")
    body = table._repr_html_()

    assert "Band planes with timelines" in body
    assert "interleaved within each pixel" in body


@pytest.mark.parametrize(
    ("shape", "pattern", "unit", "layout", "name"),
    [
        ((2, 32, 32), "b (row h) (col w) -> row col (b h w)", 1, "b h w", "Band planes"),
        ((2, 32, 32), "t (row h) (col w) -> row col (t h w)", 1, "t h w", "Time planes"),
        ((2, 32, 32), "b (row h) (col w) -> row col (h w b)", 2, "h w b", "Pixel spectra"),
        ((2, 32, 32), "t (row h) (col w) -> row col (h w t)", 2, "h w t", "Pixel timelines"),
        (
            (2, 2, 32, 32),
            "t b (row h) (col w) -> row col (b t h w)",
            3,
            "b t h w",
            "Band-first plane cube",
        ),
        (
            (2, 2, 32, 32),
            "t b (row h) (col w) -> row col (t b h w)",
            4,
            "t b h w",
            "Time-first plane cube",
        ),
        (
            (2, 2, 32, 32),
            "t b (row h) (col w) -> row col (b h w t)",
            5,
            "b h w t",
            "Band planes with timelines",
        ),
        (
            (2, 2, 32, 32),
            "t b (row h) (col w) -> row col (t h w b)",
            6,
            "t h w b",
            "Time planes with spectra",
        ),
        (
            (2, 2, 32, 32),
            "t b (row h) (col w) -> row col (h w b t)",
            7,
            "h w b t",
            "Band-first pixel cubes",
        ),
        (
            (2, 2, 32, 32),
            "t b (row h) (col w) -> row col (h w t b)",
            8,
            "h w t b",
            "Time-first pixel cubes",
        ),
    ],
)
def test_every_cell_layout_has_its_own_named_figure(shape, pattern, unit, layout, name):
    table = rumi.frames(np.zeros(shape, dtype=np.uint16), pattern, tile_size=16)
    body = table._repr_html_()

    assert table.frame_unit == unit
    assert f'data-frame-figure="{unit}:{layout}"' in body
    assert name in body
