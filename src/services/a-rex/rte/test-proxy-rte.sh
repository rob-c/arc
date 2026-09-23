#!/bin/sh

# ENV/PROXY stages the CA directory to jobs as one archive. Stage 0 runs as the
# job's mapped account, and the archive is the job's set of trust anchors, so
# check what goes into it, when a cached copy is built or reused, and that
# stage 1 unpacks it.
RTE="${PROXY_RTE:?}"

failures=0
tests=0
check () {
    tests=$((tests + 1))
    if eval "$2"; then
        printf 'ok %s - %s\n' "$tests" "$1"
    else
        printf 'not ok %s - %s\n' "$tests" "$1"
        failures=$((failures + 1))
    fi
}

top=$(mktemp -d "${TMPDIR:-/tmp}/arc-proxy-rte-test.XXXXXX") || exit 1
trap 'chmod -R u+w "$top" 2>/dev/null; rm -rf "$top"' 0

cadir="$top/certificates"
mkdir "$cadir" "$top/control" "$top/cache"
echo cert > "$cadir/ca.pem"
echo policy > "$cadir/ca.signing_policy"
ln -s ca.pem "$cadir/abcd1234.0"
echo key > "$cadir/ca-key.pem"
chmod 644 "$cadir/ca.pem" "$cadir/ca.signing_policy"
chmod 400 "$cadir/ca-key.pem"
echo proxy > "$top/control/proxy"

# What A-REX provides to stage 0.
control_path () { echo "$1/$3"; }
joboption_controldir="$top/control"
joboption_gridid=job

# Run stage 0 for a new session directory, as the submit script does: sourced.
stage0 () {
    joboption_directory="$top/session.$1"
    mkdir "$joboption_directory"
    ( X509_CERT_DIR="$cadir"; CONFIG_controldir="$top/cache"
      CACERT_TAR_CACHE="${CACERT_TAR_CACHE:-}"
      set -- 0; . "$RTE" ) >/dev/null 2>&1
}
members () { tar tf "$1" 2>/dev/null | sed 's#^\./##' | sort | tr '\n' ' '; }

stage0 a
check "stage 0 stages the CA directory as one archive" \
      '[ -f "$top/session.a/certificates.tar" ] && [ ! -d "$top/session.a/arc/certificates" ]'
check "archive holds the world-readable entries and links" \
      '[ "$(members "$top/session.a/certificates.tar")" = "abcd1234.0 ca.pem ca.signing_policy " ]'
check "an unreadable file is left out rather than failing the archive" \
      '! tar tf "$top/session.a/certificates.tar" | grep -q key'
check "proxy is staged" '[ "$(cat "$top/session.a/user.proxy")" = proxy ]'
check "reusable archive is built where its directory is writable" \
      '[ -f "$top/cache/ca-certificates.tar" ]'
check "reusable archive is readable by all" \
      '[ "$(stat -c %a "$top/cache/ca-certificates.tar")" = 644 ]'

touch -d '1 hour ago' "$cadir" "$cadir"/*
touch "$top/cache/ca-certificates.tar"
before=$(stat -c %Y "$top/cache/ca-certificates.tar")
sleep 1
stage0 b
check "an up to date archive is reused, not rebuilt" \
      '[ "$(stat -c %Y "$top/cache/ca-certificates.tar")" = "$before" ]'
check "the reused archive is staged" \
      '[ "$(cksum < "$top/cache/ca-certificates.tar")" = "$(cksum < "$top/session.b/certificates.tar")" ]'

echo cert2 > "$cadir/new.pem"; chmod 644 "$cadir/new.pem"
stage0 c
check "archive is rebuilt when the CA directory changes" \
      'tar tf "$top/session.c/certificates.tar" | grep -q new.pem'

# An archive someone else could have written is not trusted, even if current.
cp "$top/cache/ca-certificates.tar" "$top/planted.tar"
echo planted > "$top/extra"; tar rf "$top/planted.tar" -C "$top" extra
cp "$top/planted.tar" "$top/cache/ca-certificates.tar"
chmod 666 "$top/cache/ca-certificates.tar"
stage0 d
check "a group or other writable archive is not reused" \
      '! tar tf "$top/session.d/certificates.tar" | grep -q extra'
check "the job still gets the CA directory, packed directly" \
      'tar tf "$top/session.d/certificates.tar" | grep -q ca.pem'
chmod 644 "$top/cache/ca-certificates.tar"

if [ "$(id -u)" != 0 ]; then
    # Stage 0 as the mapped account: the control directory is not writable.
    rm -f "$top/cache/ca-certificates.tar"
    chmod 555 "$top/cache"
    stage0 e
    check "no archive is built in a directory this account cannot write" \
          '[ ! -f "$top/cache/ca-certificates.tar" ]'
    check "the CA directory is packed straight into the session instead" \
          'tar tf "$top/session.e/certificates.tar" | grep -q ca.pem'
    chmod 755 "$top/cache"
fi

rm -f "$top/cache/ca-certificates.tar"
env -u CACERT_TAR_CACHE X509_CERT_DIR="$cadir" CONFIG_controldir="$top/cache" \
    sh "$RTE" cache >/dev/null 2>&1
check "cache mode builds the reusable archive out of band" \
      '[ -f "$top/cache/ca-certificates.tar" ]'

# Stage 1 runs from the job script on the worker node.
( RUNTIME_JOB_DIR="$top/session.c"; set -- 1; . "$RTE"
  echo "$X509_CERT_DIR" > "$top/stage1.out" ) >/dev/null 2>&1
check "stage 1 unpacks the archive into X509_CERT_DIR" \
      '[ "$(cat "$top/stage1.out")" = "$top/session.c/arc/certificates" ] &&
       [ -f "$top/session.c/arc/certificates/new.pem" ] &&
       [ -L "$top/session.c/arc/certificates/abcd1234.0" ]'
check "stage 1 removes the unpacked archive" '[ ! -f "$top/session.c/certificates.tar" ]'

printf '1..%s\n' "$tests"
[ "$failures" -eq 0 ]
