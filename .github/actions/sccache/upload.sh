#!/usr/bin/env bash
# End of job: stop the sccache server (it prints the job's counters) and send the
# disk level's entries to the S3 store, which the job only read from. An entry
# already in the store (an S3 hit copied to disk, or one another job sent first)
# is left alone. Never fails the job: the cache is best effort.
#
# Inputs (environment): SCCACHE_BIN, SCCACHE_UPLOAD_STORE and SCCACHE_UPLOAD_REGION
# from setup.sh, AWS_ACCESS_KEY_ID and AWS_SECRET_ACCESS_KEY, RUNNER_TEMP, and
# optionally SCCACHE_UPLOAD_JOBS (parallel requests, default 4: the store answers 503 to more).
set -uo pipefail

[ -n "${SCCACHE_BIN:-}" ] || exit 0
"$SCCACHE_BIN" --stop-server || true

disk="$RUNNER_TEMP/sccache/disk"
if [ -z "${AWS_ACCESS_KEY_ID:-}" ] || [ -z "${AWS_SECRET_ACCESS_KEY:-}" ] || [ ! -d "$disk" ]; then
  echo "::notice title=sccache upload::Nothing to upload (no credentials or no disk level)."
  exit 0
fi

# shellcheck disable=SC2329 # run through xargs below
# One entry: a/b/<key> on disk is <store>/a/b/c/<key> in S3. Prints one result line.
put() {
  local file=$1 key url code
  key=${file##*/}
  url="$SCCACHE_UPLOAD_STORE/${key:0:1}/${key:1:1}/${key:2:1}/$key"
  # curl retries 429/5xx answers and timeouts with a growing delay; the NAS store
  # answers 503 when many jobs upload at once.
  code=$(curl -s -o /dev/null -w '%{http_code}' --retry 6 --max-time 60 --aws-sigv4 "aws:amz:$SCCACHE_UPLOAD_REGION:s3" \
    --user "$AWS_ACCESS_KEY_ID:$AWS_SECRET_ACCESS_KEY" -I "$url") || code=000
  if [ "$code" = 200 ]; then
    echo present
    return
  fi
  code=$(curl -s -o /dev/null -w '%{http_code}' --retry 6 --max-time 300 --aws-sigv4 "aws:amz:$SCCACHE_UPLOAD_REGION:s3" \
    --user "$AWS_ACCESS_KEY_ID:$AWS_SECRET_ACCESS_KEY" -T "$file" "$url") || code=000
  if [ "$code" = 200 ]; then
    echo "put $(stat -c %s "$file" 2>/dev/null || stat -f %z "$file")"
  else
    echo "failed $code"
  fi
}
export -f put
export SCCACHE_UPLOAD_STORE SCCACHE_UPLOAD_REGION AWS_ACCESS_KEY_ID AWS_SECRET_ACCESS_KEY

start=$SECONDS
# shellcheck disable=SC2016 # $1 is expanded by the inner bash
results=$(cd "$disk" && find . -type f -path './?/?/*' ! -path './?/?/*/*' -print0 |
  xargs -0 -r -n 1 -P "${SCCACHE_UPLOAD_JOBS:-4}" bash -c 'put "$1"' _)
entries=$(grep -c . <<<"$results")
put=$(grep -c '^put ' <<<"$results")
bytes=$(awk '$1 == "put" { s += $2 } END { print s + 0 }' <<<"$results")
present=$(grep -c '^present$' <<<"$results")
failed=$(grep -c '^failed' <<<"$results")
echo "sccache upload: $entries disk entries, $put sent ($bytes bytes), $present already in the store, $failed failed, $((SECONDS - start)) s"
if [ "$failed" -gt 0 ]; then
  statuses=$(grep '^failed' <<<"$results" | sort | uniq -c | awk '{printf "%s HTTP %s, ", $1, $3}')
  echo "::warning title=sccache upload::$failed of $entries cache entries did not reach the store (${statuses%, })"
fi
exit 0
