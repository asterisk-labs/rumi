"""Band texts and dates for tests that only need a valid file."""

import datetime as dt

FIRST_DAY = dt.date(2024, 1, 1)


def labels(tf):
    """Name every band and date every step of a frame table."""
    return {"bands": [f"band {b}" for b in range(tf.bands)],
            "time": [FIRST_DAY + dt.timedelta(days=t)
                     for t in range(tf.time_count)]}
