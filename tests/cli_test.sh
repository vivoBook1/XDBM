#!/bin/sh
# End-to-end tests for the xdbm command: output, exit codes and file
# selection. Runs in a temporary directory with HOME pointed there, so the
# real ~/.xdbm is never touched.
#
# Usage: tests/cli_test.sh [path/to/xdbm]   (default: build/xdbm)

set -u

XDBM=${1:-build/xdbm}
case $XDBM in
    /*) ;;
    *) XDBM=$PWD/$XDBM ;;
esac
if [ ! -x "$XDBM" ]; then
    echo "cli_test: $XDBM not found; run make first" >&2
    exit 1
fi

TMP=$(mktemp -d "${TMPDIR:-/tmp}/xdbm_cli_test.XXXXXX") || exit 1
trap 'rm -rf "$TMP"' EXIT
export HOME="$TMP/home"
mkdir -p "$HOME"
unset XDBM_DB
cd "$TMP" || exit 1

checks=0
failures=0

# run ARGS...: runs xdbm, setting $out, $err and $status.
run() {
    "$XDBM" "$@" >"$TMP/stdout" 2>"$TMP/stderr"
    status=$?
    out=$(cat "$TMP/stdout")
    err=$(cat "$TMP/stderr")
}

fail() {
    failures=$((failures + 1))
    {
        echo "FAIL: $1"
        echo "  stdout: $out"
        echo "  stderr: $err"
        echo "  exit:   $status"
    } >&2
}

# expect_status N LABEL
expect_status() {
    checks=$((checks + 1))
    [ "$status" -eq "$1" ] || fail "$2: expected exit $1"
}

# expect_out TEXT LABEL
expect_out() {
    checks=$((checks + 1))
    [ "$out" = "$1" ] || fail "$2: expected stdout '$1'"
}

# expect_err SUBSTRING LABEL
expect_err() {
    checks=$((checks + 1))
    case $err in
        *"$1"*) ;;
        *) fail "$2: expected stderr containing '$1'" ;;
    esac
}

# expect_no_err LABEL
expect_no_err() {
    checks=$((checks + 1))
    [ -z "$err" ] || fail "$1: expected empty stderr"
}

sorted() { printf '%s\n' "$1" | sort; }

# ---- usage ----

run help
expect_status 0 "help"
checks=$((checks + 1))
case $out in usage:*) ;; *) fail "help: expected usage text" ;; esac

run
expect_status 2 "no arguments"
run frobnicate
expect_status 2 "unknown command"
run get
expect_status 2 "get without a key"
run put onlykey
expect_status 2 "put without a value"
run firstkey
expect_status 2 "removed firstkey command"

# ---- put / get / overwrite / remove ----

export XDBM_DB="$TMP/main.db"

run put name Alice
expect_status 0 "put"
expect_no_err "put"
run get name
expect_status 0 "get"
expect_out "Alice" "get"

run put name Bob
run get name
expect_out "Bob" "overwrite"

run put greeting "hello world"
run get greeting
expect_out "hello world" "value with spaces"

run put empty ""
run get empty
expect_status 0 "empty value"
expect_out "" "empty value"

run get missing
expect_status 1 "get missing"
expect_err "not found: missing" "get missing"

run remove greeting
expect_status 0 "remove"
run get greeting
expect_status 1 "get after remove"
run remove greeting
expect_status 1 "remove twice"
expect_err "not found: greeting" "remove twice"

# ---- which database file ----

unset XDBM_DB
run put where default
expect_status 0 "default database"
checks=$((checks + 1))
[ -f "$HOME/.xdbm/data.db" ] || fail "default database: ~/.xdbm/data.db not created"

export XDBM_DB="$TMP/env.db"
run put where env
run --db "$TMP/flag.db" put where flag
run --db="$TMP/flag2.db" put where flag2
run get where
expect_out "env" "XDBM_DB is used"
run --db "$TMP/flag.db" get where
expect_out "flag" "--db overrides XDBM_DB"
run --db="$TMP/flag2.db" get where
expect_out "flag2" "--db=FILE form"
unset XDBM_DB
run get where
expect_out "default" "default database untouched by --db and XDBM_DB"

run --db
expect_status 2 "--db without a file"

printf 'not a database, just some text\n' > "$TMP/garbage.db"
run --db "$TMP/garbage.db" get x
expect_status 1 "non-database file"
expect_err "xdbm:" "non-database file"

# ---- list ----

export XDBM_DB="$TMP/list.db"

run list
expect_status 0 "list on empty database"
expect_out "" "list on empty database"

run fill 25
expect_status 0 "fill"
run stats
case $out in *"records:        25"*) ;; *) fail "stats after fill: expected 25 records" ;; esac
checks=$((checks + 1))

expected_keys=$(i=0; while [ $i -lt 25 ]; do echo "key$i"; i=$((i + 1)); done | sort)
run list
all_keys=$out
expect_status 0 "list"
checks=$((checks + 1))
# Storage order isn't sorted, so compare as sets.
[ "$(sorted "$all_keys")" = "$expected_keys" ] || fail "list: expected key0..key24"

# ---- list paging ----

run list --limit 10
expect_status 0 "list --limit"
checks=$((checks + 1))
[ "$(printf '%s\n' "$out" | wc -l | tr -d ' ')" = "10" ] || fail "list --limit 10: expected 10 keys"
first_page_last=$(printf '%s\n' "$out" | tail -n 1)
expect_err "continue with: xdbm list --after '$first_page_last' --limit 10" "list --limit hint"

# Page through everything 7 at a time; must equal plain `list`, in order.
paged=""
last=""
while :; do
    if [ -z "$last" ]; then run list --limit 7; else run list --after "$last" --limit 7; fi
    [ "$status" -eq 0 ] || { fail "paging: list exited $status"; break; }
    [ -n "$out" ] || break
    paged=$(printf '%s\n%s' "$paged" "$out" | sed '/^$/d')
    last=$(printf '%s\n' "$out" | tail -n 1)
done
checks=$((checks + 1))
[ "$paged" = "$all_keys" ] || fail "paging 7 at a time doesn't match list"

run list --limit=3
expect_out "$(printf '%s\n' "$all_keys" | head -n 3)" "list --limit=N form"

run list --limit 25
expect_no_err "list --limit covering everything prints no hint"

last_key=$(printf '%s\n' "$all_keys" | tail -n 1)
run list --after "$last_key"
expect_status 0 "list --after last key"
expect_out "" "list --after last key"

run list --after "$(printf '%s\n' "$all_keys" | head -n 1)"
expect_out "$(printf '%s\n' "$all_keys" | sed '1d')" "list --after first key"

run list --after nope
expect_status 1 "list --after missing key"
expect_err "key not found: nope" "list --after missing key"

run list --limit 0
expect_status 2 "list --limit 0"
run list --limit abc
expect_status 2 "list --limit abc"
run list --limit
expect_status 2 "list --limit without a value"
run list --bogus 1
expect_status 2 "list unknown option"

# ---- overflow chains, buckets, compact ----

export XDBM_DB="$TMP/overflow.db"

run --max-depth 1 fill 500
expect_status 0 "fill with --max-depth 1"
run buckets
expect_status 0 "buckets"
case $out in *"(overflow)"*) ;; *) fail "buckets: expected overflow pages" ;; esac
case $out in *"global depth 1, 2 buckets, 500 records"*) ;; *) fail "buckets: expected 2 buckets, 500 records" ;; esac
checks=$((checks + 2))

run get key499
expect_out "value499" "get from an overflow page"

run --max-depth x stats
expect_status 2 "--max-depth not a number"
expect_err "--max-depth needs a number" "--max-depth not a number"

export XDBM_DB="$TMP/deep.db"
run fill 1000
run --max-depth 1 stats
expect_status 1 "depth above --max-depth"
expect_err "exceeds max_global_depth" "depth above --max-depth"

export XDBM_DB="$TMP/overflow.db"
i=0
while [ $i -lt 500 ]; do
    [ $((i % 4)) -eq 0 ] || "$XDBM" remove "key$i" || fail "remove key$i"
    i=$((i + 1))
done
run compact
expect_status 0 "compact"
case $out in "removed 375 deleted records, freed "*) ;; *) fail "compact: expected 375 deleted records removed" ;; esac
checks=$((checks + 1))
run compact
expect_out "removed 0 deleted records, freed 0 overflow pages (rewrote 0 buckets)" "second compact"
run get key0
expect_out "value0" "data intact after compact"
run list
checks=$((checks + 1))
[ "$(printf '%s\n' "$out" | wc -l | tr -d ' ')" = "125" ] || fail "list after compact: expected 125 keys"

# ---- summary ----

if [ "$failures" -ne 0 ]; then
    echo "cli tests: $failures of $checks checks failed" >&2
    exit 1
fi
echo "all cli tests passed ($checks checks)"
