#!/usr/bin/env bash
# End of job: stop the sccache server (it prints the job's counters) and, on main
# only, send the disk level's new entries to the S3 store, which the job only read
# from. The store is RustFS on a NAS that also takes the nightly backup, so the
# upload is deliberately gentle:
#   - only pushes to main and manual runs on main upload; pull requests read only;
#   - nothing is sent between 02:45 and 05:00 America/Los_Angeles (the backup);
#   - it starts after a random 0-90 s delay, so jobs that end together spread out;
#   - at most 2 requests at a time, a HEAD before each PUT (no bucket listings),
#     and an entry already in the store is skipped;
#   - the first 503, refused connection, or request over 30 s stops the rest;
#   - a request is retried once only after a dropped connection.
# Never fails the job: the cache is best effort. Every skip says why.
#
# Inputs (environment): SCCACHE_BIN, SCCACHE_UPLOAD_STORE and SCCACHE_UPLOAD_REGION
# from setup.sh, AWS_ACCESS_KEY_ID and AWS_SECRET_ACCESS_KEY, RUNNER_TEMP,
# GITHUB_EVENT_NAME and GITHUB_REF from the runner. For trying the script locally:
# SCCACHE_UPLOAD_MAX_DELAY (seconds, default 90) and SCCACHE_UPLOAD_CLOCK (an HHMM
# Los Angeles time to use instead of the current one).
set -uo pipefail

[ -n "${SCCACHE_BIN:-}" ] || exit 0
"$SCCACHE_BIN" --stop-server || true

skip() {
  echo "::notice title=sccache upload skipped::$1"
  exit 0
}

disk="$RUNNER_TEMP/sccache/disk"
if [ -z "${AWS_ACCESS_KEY_ID:-}" ] || [ -z "${AWS_SECRET_ACCESS_KEY:-}" ]; then
  skip 'No store credentials.'
fi
[ -d "$disk" ] || skip 'No disk level (the cache was off in this job).'
case "${GITHUB_EVENT_NAME:-}:${GITHUB_REF:-}" in
  push:refs/heads/main|workflow_dispatch:refs/heads/main) ;;
  *) skip "Only pushes and manual runs on main upload; this is ${GITHUB_EVENT_NAME:-unknown} on ${GITHUB_REF:-unknown}. The job read the cache only." ;;
esac

# Los Angeles wall-clock time (HHMM), so daylight saving moves the window with it.
in_backup_window() {
  la_now=${SCCACHE_UPLOAD_CLOCK:-$(TZ=America/Los_Angeles date +%H%M)}
  [ "$((10#$la_now))" -ge 245 ] && [ "$((10#$la_now))" -lt 500 ]
}
if in_backup_window; then
  skip "It is ${la_now:0:2}:${la_now:2:2} in Los Angeles: no uploads between 02:45 and 05:00, the NAS backup window."
fi

entries=$(cd "$disk" && find . -type f -path './?/?/*' ! -path './?/?/*/*' | wc -l | tr -d ' ')
[ "$entries" -gt 0 ] || skip 'The disk level holds no entries.'

delay=$((RANDOM % (${SCCACHE_UPLOAD_MAX_DELAY:-90} + 1)))
echo "sccache upload: $entries disk entries; starting in $delay s"
sleep "$delay"
if in_backup_window; then
  skip "It is ${la_now:0:2}:${la_now:2:2} in Los Angeles: the NAS backup window (02:45-05:00) began during the delay."
fi

work=$(mktemp -d "$RUNNER_TEMP/sccache-upload.XXXXXX")
stop="$work/stop"

# shellcheck disable=SC2329 # used by put, through xargs
# One request: prints the HTTP status (000 when there was none); exit status is curl's.
# A dropped connection (curl 52, 55, 56) is retried once; nothing else is.
request() {
  local code rc
  code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 30 --aws-sigv4 "aws:amz:$SCCACHE_UPLOAD_REGION:s3" \
    --user "$AWS_ACCESS_KEY_ID:$AWS_SECRET_ACCESS_KEY" "$@")
  rc=$?
  case $rc in
    52|55|56)
      code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 30 --aws-sigv4 "aws:amz:$SCCACHE_UPLOAD_REGION:s3" \
        --user "$AWS_ACCESS_KEY_ID:$AWS_SECRET_ACCESS_KEY" "$@")
      rc=$? ;;
  esac
  echo "${code:-000}"
  return "$rc"
}

# shellcheck disable=SC2329 # used by put, through xargs
# Stop every remaining upload of this job, keeping the first reason.
halt() {
  [ -e "$stop" ] || echo "$1" > "$stop"
}

# shellcheck disable=SC2329 # run through xargs below
# One entry: a/b/<key> on disk is <store>/a/b/c/<key> in S3. Prints one result line.
put() {
  local file=$1 key url code rc
  if [ -e "$stop" ]; then echo stopped; return; fi
  key=${file##*/}
  url="$SCCACHE_UPLOAD_STORE/${key:0:1}/${key:1:1}/${key:2:1}/$key"
  code=$(request -I "$url"); rc=$?
  case "$rc:$code" in
    0:200) echo present; return ;;
    0:404) ;;
    28:*) halt "a HEAD took over 30 s"; echo "failed timeout"; return ;;
    7:*) halt "the store refused the connection"; echo "failed refused"; return ;;
    *:503) halt "the store answered 503"; echo "failed 503"; return ;;
    *) echo "failed $code"; return ;;
  esac
  if [ -e "$stop" ]; then echo stopped; return; fi
  code=$(request -T "$file" "$url"); rc=$?
  case "$rc:$code" in
    0:200) echo "put $(stat -c %s "$file" 2>/dev/null || stat -f %z "$file")" ;;
    28:*) halt "a PUT took over 30 s"; echo "failed timeout" ;;
    7:*) halt "the store refused the connection"; echo "failed refused" ;;
    *:503) halt "the store answered 503"; echo "failed 503" ;;
    *) echo "failed $code" ;;
  esac
}
export -f request halt put
export stop SCCACHE_UPLOAD_STORE SCCACHE_UPLOAD_REGION AWS_ACCESS_KEY_ID AWS_SECRET_ACCESS_KEY

start=$SECONDS
# shellcheck disable=SC2016 # $1 is expanded by the inner bash
results=$(cd "$disk" && find . -type f -path './?/?/*' ! -path './?/?/*/*' -print0 |
  xargs -0 -r -n 1 -P 2 bash -c 'put "$1"' _)
count() { grep -c "$1" <<<"$results"; }
put=$(count '^put ')
bytes=$(awk '$1 == "put" { s += $2 } END { print s + 0 }' <<<"$results")
echo "sccache upload: $entries disk entries, $put sent ($bytes bytes), $(count '^present$') already in the store," \
  "$(count '^failed') failed, $(count '^stopped$') not tried, $((SECONDS - start)) s"
if [ -e "$stop" ]; then
  echo "::warning title=sccache upload stopped::Stopped early because $(cat "$stop"); the rest of this job's entries were not sent."
elif [ "$(count '^failed')" -gt 0 ]; then
  statuses=$(grep '^failed' <<<"$results" | sort | uniq -c | awk '{printf "%s HTTP %s, ", $1, $3}')
  echo "::warning title=sccache upload::$(count '^failed') of $entries cache entries did not reach the store (${statuses%, })"
fi
rm -rf "$work"
exit 0
