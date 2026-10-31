#!/bin/sh
set -eu

log=${1:?R CMD check log is required}
[ -r "$log" ] || { echo "cannot read $log" >&2; exit 1; }

count=$(grep -c '^Status: ' "$log" || true)
[ "$count" -eq 1 ] || { echo "missing or repeated R CMD check status" >&2; exit 1; }
status=$(sed -n 's/^Status: //p' "$log")
[ "$status" = OK ] && { echo "R CMD check: OK"; exit 0; }

other=$(grep -E '\.\.\. (NOTE|WARNING|ERROR)$' "$log" |
    grep -v 'checking compiled code' || true)
objects=$(grep -E 'Objects?:' "$log" || true)
bad=$(printf '%s\n' "$objects" |
    grep -Eo 'lib/lib[[:alnum:]_.+-]+\.a' |
    grep -Ev '^lib/lib(openzl|zstd|lz4)\.a$' || true)

case "$status" in
    "1 NOTE"|"1 WARNING") ;;
    *) echo "R CMD check: $status" >&2; exit 1 ;;
esac
[ -z "$other" ] && [ -n "$objects" ] && [ -z "$bad" ] &&
    grep -q 'checking compiled code .* \(NOTE\|WARNING\)' "$log" &&
    ! grep -q 'rumi_r\.o' "$log" || {
        echo "R CMD check: unexpected finding" >&2
        [ -n "$other" ] && echo "$other" >&2
        exit 1
    }
echo "R CMD check: $status from vendored archives"
