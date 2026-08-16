# Compatibility

RUMI 0.15 is the current format baseline. Readers in the 0.15 series will keep
opening canonical files and external headers written by 0.15.0. The experimental
0.14 format and earlier versions are incompatible.

## APIs

The public C source API is stable from 0.15. The binary ABI is not stable before
1.0, so C applications should be recompiled after an update. ABI changes also
increment `RUMI_API_VERSION` and the shared-library SONAME.

The Python API follows semantic versioning. Before 1.0, a minor release may
contain a breaking change; it will be called out in the changelog.

## GeoZL and OpenZL

RUMI 0.15.x includes GeoZL 0.13.1 and OpenZL 0.2.0. Python writing accepts
GeoZL 0.13.x.
