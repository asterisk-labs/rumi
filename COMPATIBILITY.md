# Compatibility

RUMI 0.17 is the current compatibility baseline. Readers from 0.17 onward will
keep opening canonical files written by 0.17.0. Earlier versions are not
supported.

A rumi file names its own frame layout, so any 0.17 file can be read from the
path alone. The header a writer returns saves the parse; it is not needed to get
the right samples.

## Writers and readers

Only files created by rumi's writer are supported. The writer is available as
`rumi.write` in Python and `rumi_write` in C. Independent writers are outside
the compatibility policy.

Independent readers are supported. [SPEC.md](SPEC.md) contains everything
needed to read a `.rumi` file without this library. If a file written by rumi
cannot be read from the specification, that is a specification bug.

A `.rumi` file does not record which writer created it. The reader therefore
accepts any file that follows the format, but support is limited to files
created with rumi's own writer.

## APIs

The public C source API is stable from 0.17. The binary ABI is not stable before
1.0, so C applications should be recompiled after an update. ABI changes also
increment `RUMI_API_VERSION` and the shared-library SONAME.

The Python API follows semantic versioning. Before 1.0, a minor release may
contain a breaking change; it will be called out in the changelog.

## GeoZL and OpenZL

RUMI 0.19.x uses GeoZL 0.16.x as its frame compatibility baseline and includes
OpenZL 0.2.0. The Python `write` extra requires GeoZL 0.16.x.
