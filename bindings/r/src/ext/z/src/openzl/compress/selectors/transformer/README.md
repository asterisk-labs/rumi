# Compression Transformer selector

This directory contains OpenZL's runtime integration of the Compression
Transformer numeric selector. The scorers in `generated/` are produced by a
separate training pipeline; changes that can affect feature values or model
decisions must be validated there before being synchronized here.

Generated scorer snapshots live in `generated/`; the surrounding files are
the hand-written OpenZL runtime integration.

## OpenZL adaptation

The upstream generated scorer is a starting point rather than a file copied
verbatim. The OpenZL integration currently adds:

- `TRS_` prefixes for internal symbols;
- OpenZL include paths and assertion macros;
- operation-ID tables used by typed runtime dispatch; and
- the OpenZL feature extraction, workspace, and compatibility interfaces.

The upstream generator does not yet emit every adaptation above. Preserve them
when importing a newly generated model, and run the fast/full feature and
decision parity tests after synchronization.

The dense-range score guard intentionally preserves the upstream runtime's
float-rounded `0.90f` cutoff. Replacing it with a double literal changes routing
at and just below 90% density; evaluate that change upstream before
synchronizing it here.
