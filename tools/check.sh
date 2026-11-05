#!/usr/bin/env bash
# Pulsar dev check: build in the "juce" distrobox and run the CLI tests,
# printing only what needs attention. Full logs stay in build/check-logs/.
#
#   tools/check.sh            build + smoketest + selftest
#   tools/check.sh build      build only
#   tools/check.sh smoke      smoketest only (no build)
#   tools/check.sh self       selftest only (no build)
#   tools/check.sh strict     build + smoketest + selftest --strict-cpu
#
# Exit code is non-zero if any step fails.

set -u
cd "$(dirname "$0")/.."

BUILD_DIR=build/linux-release
LOG_DIR=build/check-logs
CLI="$BUILD_DIR/vibesynth_cli"
mkdir -p "$LOG_DIR"

run_in_box() {
    if [ -f /run/.containerenv ] || [ -n "${CONTAINER_ID:-}" ]; then
        "$@"
    else
        distrobox enter juce -- "$@"
    fi
}

step_build() {
    local log="$LOG_DIR/build.log"
    if [ ! -f "$BUILD_DIR/build.ninja" ]; then
        run_in_box cmake --preset linux-release >"$LOG_DIR/configure.log" 2>&1 || {
            echo "CONFIGURE FAILED (full log: $LOG_DIR/configure.log)"
            grep -E "CMake Error|error" "$LOG_DIR/configure.log" | head -n 30
            return 1
        }
    fi
    if run_in_box cmake --build "$BUILD_DIR" >"$log" 2>&1; then
        # Warnings from our own code only (not JUCE, WDL or DaisySP).
        local warns
        local root
        root=$(pwd)
        warns=$(grep -E "^(${root}/|\.\./\.\./)?(src|tools)/[^:]+:[0-9]+:[0-9]+: warning:" "$log" \
                | sed "s|^${root}/||; s|^\.\./\.\./||" | sort -u)
        if [ -n "$warns" ]; then
            echo "BUILD OK, $(printf '%s\n' "$warns" | wc -l) warning(s) in our code:"
            printf '%s\n' "$warns" | head -n 40
        else
            echo "BUILD OK (no warnings in src/ or tools/)"
        fi
        return 0
    fi
    echo "BUILD FAILED (full log: $log)"
    # The failing commands, then the first errors with 3 lines of context.
    grep -E "^FAILED:" "$log" | head -n 10
    grep -n -E "error:|error [A-Z][0-9]+|undefined reference|ld: |fatal" -A3 "$log" | head -n 60
    return 1
}

# Prints every line that isn't a plain [PASS] line, plus a pass/fail count.
filter_test() {
    local log="$1"
    local pass fail
    pass=$(grep -c "^\[PASS\]" "$log")
    fail=$(grep -c "^\[FAIL\]" "$log")
    grep -v "^\[PASS\]" "$log"
    echo "-- $pass passed, $fail failed (full log: $log)"
}

step_test() {
    local name="$1"; shift
    local log="$LOG_DIR/$name.log"
    [ -x "$CLI" ] || { echo "$CLI not built"; return 1; }
    run_in_box ${PIN_CORE:+taskset -c "$PIN_CORE"} "$CLI" "$@" >"$log" 2>&1
    local rc=$?
    echo "== $name ($*): exit $rc"
    filter_test "$log"
    return $rc
}

# The CPU budget gate. Bloom Pulse sits within about 2% of its budget, and timing
# noise lasts for a whole process (measured 291-309 ns, lowest of 3 tries in one
# process), so a run that fails on budgets alone gets up to 2 fresh processes.
# Any other failure stops at once. A preset that is really over fails all 3.
step_strict_selftest() {
    local attempt other
    for attempt in 1 2 3; do
        # One core: moving between cores adds noise to the timings.
        PIN_CORE=2 step_test selftest --selftest --strict-cpu && return 0
        other=$(grep "^\[FAIL\]" "$LOG_DIR/selftest.log" | grep -v "fits its CPU budget" \
                | grep -vE "^\[FAIL\] [0-9]+/[0-9]+ built-in presets pass$")
        [ -n "$other" ] && return 1
        [ "$attempt" -lt 3 ] && echo "Only CPU budgets failed: measuring again in a new process ($((attempt + 1)) of 3)"
    done
    return 1
}

rc=0
case "${1:-all}" in
    build)  step_build || rc=1 ;;
    smoke)  step_test smoketest --smoketest || rc=1 ;;
    self)   step_test selftest --selftest || rc=1 ;;
    strict)
        step_build || exit 1
        step_test smoketest --smoketest || rc=1
        step_strict_selftest || rc=1 ;;
    all)
        step_build || exit 1
        step_test smoketest --smoketest || rc=1
        step_test selftest --selftest || rc=1 ;;
    *) echo "usage: $0 [all|build|smoke|self|strict]"; exit 2 ;;
esac
exit $rc
