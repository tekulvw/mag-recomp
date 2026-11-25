#!/usr/bin/env bash
# Start a pinned sccache server for this job, backed by the self-hosted runners'
# S3 store, and route later compiles through it. Never fails the job over the
# cache itself: without credentials (a fork pull request, a hosted runner, unset
# secrets), or when the store does not answer, it says so and the job compiles
# uncached. Only a wrong archive hash fails, since that is not a cache problem.
#
# Inputs (environment): AWS_ACCESS_KEY_ID, AWS_SECRET_ACCESS_KEY, SCCACHE_SETUP_PREFIX
# (the job's key prefix), SCCACHE_SETUP_ENDPOINT, SCCACHE_SETUP_BUCKET,
# SCCACHE_SETUP_WRAP ('true': put cc/c++/gcc/g++/clang/clang++ wrappers first on
# PATH instead of setting the CMake launchers), and GITHUB_ENV, GITHUB_PATH and
# RUNNER_TEMP from the runner. SCCACHE_SETUP_BINARY uses an existing sccache
# instead of the pinned download (for trying this script off Linux).
set -uo pipefail

# 0.8.2 on purpose: it is the last release whose server answers a cache miss
# while the object is still being stored. From 0.9.0 the compile waits for the
# S3 PUT, and the NAS-backed store takes seconds per PUT under CI load.
VERSION=0.8.2
ARCHIVE="sccache-v${VERSION}-x86_64-unknown-linux-musl"
# The release's own .sha256 file, checked against a download.
SHA256=ecda4ddc89a49f1ec6f35bdce5ecbf6f205b399a680d11119d4ce9f6d962104e

dir="$RUNNER_TEMP/sccache"
bin=''

off() {
  echo "::notice title=sccache off::$1. This job compiles without the shared compiler cache."
  if [ -n "$bin" ]; then "$bin" --stop-server >/dev/null 2>&1 || true; fi
  exit 0
}

if [ -z "${AWS_ACCESS_KEY_ID:-}" ] || [ -z "${AWS_SECRET_ACCESS_KEY:-}" ]; then
  off 'No cache credentials (a fork pull request, or the SCCACHE_AWS_* secrets are unset)'
fi
mkdir -p "$dir"

if [ -n "${SCCACHE_SETUP_BINARY:-}" ]; then
  bin="$SCCACHE_SETUP_BINARY"
else
  archive="$dir/$ARCHIVE.tar.gz"
  curl -fsSL --retry 3 --max-time 120 -o "$archive" \
    "https://github.com/mozilla/sccache/releases/download/v${VERSION}/${ARCHIVE}.tar.gz" ||
    off "Downloading sccache ${VERSION} failed"
  if ! echo "$SHA256  $archive" | sha256sum -c --quiet -; then
    echo "::error title=sccache::sccache ${VERSION} archive hash mismatch"
    exit 1
  fi
  tar -xzf "$archive" -C "$dir" --strip-components=1 "$ARCHIVE/sccache" || off 'Unpacking sccache failed'
  rm -f "$archive"
  bin="$dir/sccache"
fi

# Server-side settings. The credentials stay in the server's environment only;
# later steps talk to the running server and never see them.
export SCCACHE_BUCKET="$SCCACHE_SETUP_BUCKET"
export SCCACHE_ENDPOINT="$SCCACHE_SETUP_ENDPOINT"
export SCCACHE_REGION=us-east-1
export SCCACHE_S3_USE_SSL=false
export SCCACHE_S3_KEY_PREFIX="$SCCACHE_SETUP_PREFIX"
export SCCACHE_IDLE_TIMEOUT=0
export SCCACHE_ERROR_LOG="$dir/server.log"
export SCCACHE_LOG=warn
timeout 60 "$bin" --start-server || off 'The sccache server did not start'

# One real compile through the server, then its counters: the store must have
# been read without errors and be the S3 bucket, not a local fallback.
probe="$dir/probe"
mkdir -p "$probe"
printf 'int sccache_ci_probe(int x) { return x * 3 + 1; }\n' > "$probe/probe.c"
cc=$(command -v cc || command -v gcc || command -v clang) || off 'No C compiler for the probe'
(cd "$probe" && timeout 60 "$bin" "$cc" -c probe.c -o probe.o) || off 'The probe compile through sccache failed'
stats=$(timeout 30 "$bin" --show-stats) || off 'sccache --show-stats failed'
if ! grep -qE '^Cache location +s3' <<<"$stats"; then
  printf '%s\n' "$stats"
  off 'sccache did not select the S3 store'
fi
if grep -qE '^Cache read errors +[1-9]' <<<"$stats"; then
  printf '%s\n' "$stats"
  cat "$SCCACHE_ERROR_LOG" 2>/dev/null | tail -20
  off "The S3 store at $SCCACHE_ENDPOINT did not answer the probe"
fi
grep -E '^Cache location' <<<"$stats"

{
  echo "SCCACHE_BIN=$bin"
  # A lost server makes a compile run uncached instead of failing it.
  echo 'SCCACHE_IGNORE_SERVER_IO_ERROR=1'
  echo 'SCCACHE_IDLE_TIMEOUT=0'
  echo "RUSTC_WRAPPER=$bin"
  # sccache cannot cache incremental crates, and CI never reuses incremental state.
  echo 'CARGO_INCREMENTAL=0'
} >> "$GITHUB_ENV"

if [ "${SCCACHE_SETUP_WRAP:-false}" = true ]; then
  # Commands that name the compiler directly (the test suites' cc/gcc/clang calls)
  # reach sccache through these. Compile-and-link and preprocess-only calls pass
  # straight through to the compiler; only -c compiles are cached.
  wrap="$dir/bin"
  mkdir -p "$wrap"
  for name in cc c++ gcc g++ clang clang++; do
    real=$(command -v "$name") || continue
    printf '#!/bin/sh\nexec "%s" "%s" "$@"\n' "$bin" "$real" > "$wrap/$name"
    chmod +x "$wrap/$name"
    echo "wrapped $name -> $real"
  done
  echo "$wrap" >> "$GITHUB_PATH"
else
  {
    echo "CMAKE_C_COMPILER_LAUNCHER=$bin"
    echo "CMAKE_CXX_COMPILER_LAUNCHER=$bin"
  } >> "$GITHUB_ENV"
fi
echo "sccache ${VERSION} on, key prefix $SCCACHE_SETUP_PREFIX"
