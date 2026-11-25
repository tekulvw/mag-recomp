#!/usr/bin/env bash
# Start a pinned sccache server for this job, with a local disk level in front of
# the self-hosted runners' S3 store (read-only until upload.sh runs at the end of
# the job), and route later compiles through it. Never fails the job over the
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

# Every release since 0.9.0 answers a cache miss only after the object is
# stored, so on a miss the compile waits for the S3 PUT; the NAS-backed store
# takes seconds per PUT under CI load. 0.8.2 was the last release that did not.
VERSION=0.20.1
ARCHIVE="sccache-v${VERSION}-x86_64-unknown-linux-musl"
# The release's own .sha256 file, checked against a download.
SHA256=c9f6c5a3ae84d9da0956b28c357a365827bda33f3383647fc0b67484c74a5ea9

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
[ -n "${SCCACHE_SETUP_PREFIX:-}" ] || off 'No key prefix given'
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

# Two levels: a miss is stored on local disk only (L0), and S3 (L1) is read-only
# while the job runs, so no compile waits for an upload (sccache 0.9.0 and later
# answer a miss only after storing it, and the NAS store takes seconds per PUT).
# upload.sh sends this job's new disk entries to S3 when the job ends. The two
# backends name an entry alike: disk a/b/<key>, S3 <prefix>/a/b/c/<key>.
# The credentials stay in the server's environment (and the upload step's).
region=us-east-1
store="${SCCACHE_SETUP_ENDPOINT%/}/$SCCACHE_SETUP_BUCKET/$SCCACHE_SETUP_PREFIX"
export SCCACHE_MULTILEVEL_CHAIN=disk,s3
export SCCACHE_DIR="$dir/disk"
export SCCACHE_S3_RW_MODE=READ_ONLY
export SCCACHE_BUCKET="$SCCACHE_SETUP_BUCKET"
export SCCACHE_ENDPOINT="$SCCACHE_SETUP_ENDPOINT"
export SCCACHE_REGION="$region"
export SCCACHE_S3_USE_SSL=false
export SCCACHE_S3_KEY_PREFIX="$SCCACHE_SETUP_PREFIX"
# A slow store turns a lookup into a miss instead of stalling the compile.
export SCCACHE_CACHE_READ_TIMEOUT_SECS=5
export SCCACHE_IDLE_TIMEOUT=0
export SCCACHE_ERROR_LOG="$dir/server.log"
export SCCACHE_LOG=warn
# The server's own storage check runs inside the client's 10 s startup wait, which the
# NAS store has overrun under load; the signed request below checks the store instead.
export SCCACHE_SKIP_CACHE_CHECK=1

# The store must answer these credentials: 200 or 404 for an object under the prefix.
# (A multi-level cache counts a refused read as a miss, so the counters cannot tell.)
# curl retries a 503 (the NAS store's answer when busy) with a growing delay.
code=$(curl -sS -o /dev/null -w '%{http_code}' --retry 5 --max-time 20 --aws-sigv4 "aws:amz:$region:s3" \
  --user "$AWS_ACCESS_KEY_ID:$AWS_SECRET_ACCESS_KEY" -I "$store/.sccache_check") || code=000
case "$code" in
  200|404) ;;
  *) off "The S3 store at $SCCACHE_SETUP_ENDPOINT refused or did not answer (HTTP $code)" ;;
esac
timeout 60 "$bin" --start-server || off 'The sccache server did not start'

# One real compile through the server, and the levels it reports.
probe="$dir/probe"
mkdir -p "$probe"
printf 'int sccache_ci_probe(int x) { return x * 3 + 1; }\n' > "$probe/probe.c"
cc=$(command -v cc || command -v gcc || command -v clang) || off 'No C compiler for the probe'
(cd "$probe" && timeout 60 "$bin" "$cc" -c probe.c -o probe.o) || off 'The probe compile through sccache failed'
stats=$(timeout 30 "$bin" --show-stats) || off 'sccache --show-stats failed'
if ! grep -qE '^ +L1 \(s3\) +s3, name:' <<<"$stats"; then
  printf '%s\n' "$stats"
  off 'sccache did not set up the disk and S3 levels'
fi
grep -E '^ +L[01] \((disk|s3)\) +(Local disk|s3, name)' <<<"$stats"

{
  echo "SCCACHE_BIN=$bin"
  # For upload.sh at the end of the job (not secret).
  echo "SCCACHE_UPLOAD_STORE=$store"
  echo "SCCACHE_UPLOAD_REGION=$region"
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
