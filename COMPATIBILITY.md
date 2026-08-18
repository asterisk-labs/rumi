# Compatibility

RUMI 0.16 is the current compatibility baseline. Readers from 0.16 onward will
keep opening canonical files written by 0.16.0. Earlier versions are not
supported.

## APIs

The public C source API is stable from 0.16. The binary ABI is not stable before
1.0, so C applications should be recompiled after an update. ABI changes also
increment `RUMI_API_VERSION` and the shared-library SONAME.

The Python API follows semantic versioning. Before 1.0, a minor release may
contain a breaking change; it will be called out in the changelog.

## GeoZL and OpenZL

RUMI 0.16.x uses GeoZL 0.14.x as its frame compatibility baseline and includes
OpenZL 0.2.0. Python writing requires GeoZL 0.14.x.
