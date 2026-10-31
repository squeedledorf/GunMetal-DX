#!/usr/bin/env bash
# Run, replay, minimise or merge the libFuzzer harnesses (indra/fuzz, built with the linux-fuzz preset).
#
#   scripts/fuzz/run.sh list                          the harnesses that are built
#   scripts/fuzz/run.sh <target> [seconds]            fuzz for N seconds (default 60)
#   scripts/fuzz/run.sh all [seconds]                 every harness, N seconds each
#   scripts/fuzz/run.sh check [target]                replay the seed corpus once (no mutation; fast, CI smoke)
#   scripts/fuzz/run.sh repro <target> <file>...      run specific inputs (a crash artifact) under the sanitizers
#   scripts/fuzz/run.sh minimize <target> <crash>     shrink a crashing input to <crash>.min
#   scripts/fuzz/run.sh merge <target>                fold the work corpus into a minimal set, back into the work dir
#
# <target> is the binary name (fuzz_llsd_xml) or the short form (llsd_xml).
#
# Environment:
#   FUZZ_BUILD_DIR   build tree (default <repo>/build-linux-fuzz)
#   FUZZ_RSS_MB      libFuzzer -rss_limit_mb (default 2048: a tiny input that makes a decoder
#                    allocate gigabytes is a finding, not noise)
#   FUZZ_MAX_LEN     libFuzzer -max_len (default per target)
#   FUZZ_EXTRA       extra libFuzzer flags, e.g. FUZZ_EXTRA="-jobs=4 -workers=4"
#   ASAN_OPTIONS / UBSAN_OPTIONS   appended to the defaults below
#   FUZZ_LOG=1       send LL_WARNS and above to stderr (debugging a crash input)
#
# State lives under <build>/fuzz-work/<name>/: corpus/ (grown by the fuzzer), crashes/ (artifacts),
# run/ (cwd). The checked-in seeds in indra/fuzz/corpus/<name>/ are only ever read, never written.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD="${FUZZ_BUILD_DIR:-$ROOT/build-linux-fuzz}"
BIN_DIR="$BUILD/fuzz"
WORK_ROOT="$BUILD/fuzz-work"
SEEDS_ROOT="$ROOT/indra/fuzz/corpus"
DICT_ROOT="$ROOT/indra/fuzz/dict"

die() { echo "fuzz/run.sh: $*" >&2; exit 2; }

list_targets() {
    [ -d "$BIN_DIR" ] || die "no harnesses in $BIN_DIR; build them: cmake -S indra --preset linux-fuzz && ninja -C build-linux-fuzz fuzz_all"
    local f
    for f in "$BIN_DIR"/fuzz_*; do
        if [ -f "$f" ] && [ -x "$f" ]; then basename "$f"; fi
    done
}

# sets BIN, NAME, SEEDS, DICT, CORPUS, CRASHES, RUNDIR, MAXLEN
resolve() {
    local t="$1"
    case "$t" in fuzz_*) ;; *) t="fuzz_$t" ;; esac
    BIN="$BIN_DIR/$t"
    [ -x "$BIN" ] || die "$BIN not built (targets: $(list_targets | tr '\n' ' '))"
    NAME="${t#fuzz_}"
    SEEDS="$SEEDS_ROOT/$NAME"
    DICT="$DICT_ROOT/$NAME.dict"
    CORPUS="$WORK_ROOT/$NAME/corpus"
    CRASHES="$WORK_ROOT/$NAME/crashes"
    RUNDIR="$WORK_ROOT/$NAME/run"
    mkdir -p "$CORPUS" "$CRASHES" "$RUNDIR"
    case "$NAME" in
        message)                 MAXLEN=1600 ;;
        image_*|mesh_*|unzip_llsd) MAXLEN=262144 ;;
        *)                       MAXLEN=65536 ;;
    esac
    MAXLEN="${FUZZ_MAX_LEN:-$MAXLEN}"
}

setup_env() {
    # protect_shadow_gap=0: the NVIDIA driver maps into ASan's shadow gap (the harnesses load no
    # GL, but keep one value for everything run on this machine). detect_leaks=0: LL's lazily grown
    # tables and singletons would report per-input leaks.
    export ASAN_OPTIONS="protect_shadow_gap=0:detect_leaks=0:symbolize=1:strict_string_checks=1${ASAN_OPTIONS:+:$ASAN_OPTIONS}"
    export UBSAN_OPTIONS="suppressions=$ROOT/indra/fuzz/ubsan.supp:halt_on_error=1:print_stacktrace=1${UBSAN_OPTIONS:+:$UBSAN_OPTIONS}"
}

common_flags() {
    FLAGS=(-detect_leaks=0 "-rss_limit_mb=${FUZZ_RSS_MB:-2048}" "-max_len=$MAXLEN" -timeout=30)
    if [ -f "$DICT" ]; then FLAGS+=("-dict=$DICT"); fi
}

run_one() {   # <target> <seconds>
    resolve "$1"; common_flags
    echo "== $NAME: ${2}s, corpus $CORPUS (seeds $SEEDS), crashes -> $CRASHES"
    # shellcheck disable=SC2086
    ( cd "$RUNDIR" && "$BIN" "${FLAGS[@]}" "-max_total_time=$2" "-artifact_prefix=$CRASHES/" \
        -print_final_stats=1 ${FUZZ_EXTRA:-} "$CORPUS" "$SEEDS" )
}

cmd="${1:-}"
[ -n "$cmd" ] || { sed -n '2,12p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 2; }
shift
setup_env

case "$cmd" in
    list)
        list_targets
        ;;
    all)
        secs="${1:-60}"; rc=0
        for t in $(list_targets); do run_one "$t" "$secs" || rc=$?; done
        exit $rc
        ;;
    check)
        # one pass over the checked-in seeds (incl. crash-* regressions): catches a regression on a known input
        targets="${1:-$(list_targets)}"; rc=0
        for t in $targets; do
            resolve "$t"; common_flags
            echo "== $NAME: replay"
            ( cd "$RUNDIR" && "$BIN" "${FLAGS[@]}" -runs=0 "$SEEDS" ) || rc=$?
        done
        exit $rc
        ;;
    repro)
        resolve "${1:?usage: repro <target> <file>...}"; shift
        [ "$#" -gt 0 ] || die "usage: repro <target> <file>..."
        ( cd "$RUNDIR" && "$BIN" -detect_leaks=0 -runs=1 "$@" )
        ;;
    minimize)
        resolve "${1:?usage: minimize <target> <crash-file>}"; crash="$(readlink -f "${2:?usage: minimize <target> <crash-file>}")"
        common_flags
        # shellcheck disable=SC2086
        ( cd "$RUNDIR" && "$BIN" "${FLAGS[@]}" -minimize_crash=1 -runs=100000 "-exact_artifact_path=$crash.min" \
            ${FUZZ_EXTRA:-} "$crash" )
        echo "minimised input: $crash.min"
        echo "add it as a regression: cp $crash.min $SEEDS/crash-<short-description>.bin"
        ;;
    merge)
        resolve "${1:?usage: merge <target>}"; common_flags
        merged="$WORK_ROOT/$NAME/merged"; rm -rf "$merged"; mkdir -p "$merged"
        ( cd "$RUNDIR" && "$BIN" "${FLAGS[@]}" -merge=1 "$merged" "$CORPUS" "$SEEDS" )
        rm -rf "$CORPUS"; mv "$merged" "$CORPUS"
        echo "merged corpus: $(find "$CORPUS" -type f | wc -l) inputs"
        ;;
    *)
        run_one "$cmd" "${1:-60}"
        ;;
esac
