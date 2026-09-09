#!/usr/bin/env bash

set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: $0 INSTALL_PREFIX" >&2
  exit 2
fi

readonly curl_version=8.22.0
readonly curl_sha256=f7ef3ae8a22e521f289803fe93543eb64c329b58aa73a9e224dfd915a2a5f4f7
readonly install_prefix=$1
readonly work_dir=$(mktemp -d)
readonly archive="$work_dir/curl.tar.xz"
readonly source_dir="$work_dir/curl-$curl_version"

cleanup() {
  rm -rf "$work_dir"
}
trap cleanup EXIT

curl -fsSLo "$archive" "https://curl.se/download/curl-$curl_version.tar.xz"
echo "$curl_sha256  $archive" | sha256sum --check --status
tar -xf "$archive" -C "$work_dir"

# Karu only needs HTTP(S). A small static libcurl keeps the Linux wheel's
# transport implementation current without inheriting a distribution's old
# libcurl or pulling unrelated protocol runtimes into the wheel.
cmake -S "$source_dir" -B "$work_dir/build" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$install_prefix" \
  -DCMAKE_INSTALL_LIBDIR=lib \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DBUILD_SHARED_LIBS=OFF \
  -DBUILD_CURL_EXE=OFF \
  -DBUILD_EXAMPLES=OFF \
  -DBUILD_LIBCURL_DOCS=OFF \
  -DBUILD_MISC_DOCS=OFF \
  -DBUILD_TESTING=OFF \
  -DHTTP_ONLY=ON \
  -DCURL_USE_OPENSSL=ON \
  -DCURL_ZLIB=OFF \
  -DCURL_BROTLI=OFF \
  -DCURL_ZSTD=OFF \
  -DUSE_NGHTTP2=OFF \
  -DUSE_LIBIDN2=OFF \
  -DCURL_USE_LIBPSL=OFF \
  -DCURL_USE_LIBSSH2=OFF \
  -DCURL_USE_GSSAPI=OFF \
  -DOPENSSL_ROOT_DIR="${CONDA_PREFIX:?activate the build environment first}"

cmake --build "$work_dir/build" --target libcurl_static
cmake --install "$work_dir/build"
