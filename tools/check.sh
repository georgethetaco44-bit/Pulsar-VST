#!/usr/bin/env bash
# Pulsar dev check: build in the "juce" distrobox and run the CLI tests,
# printing only what needs attention. Full logs stay in build/check-logs/.
#
#   tools/check.sh            build + smoketest + selftest
#   tools/check.sh build      build only
#   tools/check.sh smoke      smoketest only (no build)
#   tools/check.sh self       selftest only (no build)
#   tools/check.sh strict     build + smoketest + selftest --strict-cpu at
#                             several code positions (SPEC 6.4; Linux x86-64)
#
# Exit code is non-zero if any step fails.

set -u
SELF="$(cd "$(dirname "$0")" && pwd)/$(basename "$0")"
cd "$(dirname "$0")/.."

BUILD_DIR=build/linux-release
LOG_DIR=build/check-logs
CLI="$BUILD_DIR/vibesynth_cli"
mkdir -p "$LOG_DIR"

# The strict CPU gate (SPEC 6.4). A function's cost can change 1.5-5x with
# where its machine code lands modulo 8 KB, and the launch randomises bit 12
# of that, so one process measures one placement, not the patch. The strict
# check therefore runs the selftest once per code position: copies of
# vibesynth_cli relinked with a padding object first, so all their code
# moves by whole KB, each started with address randomisation off
# (setarch -R; the base then has bit 12 clear, so +4..+7 KB are the bit-12
# flip a normal launch can land on). 0-7 KB covers every KB step of the 8 KB
# period; 1 KB steps were seen to move single functions between their fast
# and slow cost (Nov 2026 study). A preset passes on its best position; its
# worst is printed as a note. Every other test must pass at every position.
# Only this check builds or uses the copies (build/linux-release/
# strict-positions/); the normal build, CI and the plugin never see them.
STRICT_SHIFTS_KB="0 1 2 3 4 5 6 7"
STRICT_CORE=2           # one core: moving between cores adds noise
STRICT_WARMUP_S=6       # the clock dips for a few seconds after idle
POS_DIR="$BUILD_DIR/strict-positions"

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

position_cli() {
    if [ "$1" = 0 ]; then echo "$CLI"; else echo "$POS_DIR/vibesynth_cli_${1}k"; fi
}

# Inside the box: relink vibesynth_cli once per shift (only when the CLI is
# newer than the copy), using the build's own link command with a padding
# object first. The pad sits in .text.unlikely, the first group of the
# linker's .text, so every code section after it moves. Then check with nm
# that the CLI's main, the first engine function and EEL2's code_execute all
# moved by exactly the shift.
box_build_positions() {
    cd "$BUILD_DIR" || return 1
    mkdir -p strict-positions
    local link
    link=$(ninja -t commands vibesynth_cli | tail -n 1)
    case "$link" in
        ": && "*" -o vibesynth_cli "*) ;;
        *) echo "strict: can't read vibesynth_cli's link command from ninja"; return 1 ;;
    esac
    local engine_sym
    engine_sym=$(nm --defined-only libvibesynth_engine.a 2>/dev/null \
                 | awk '$2 == "T" && $3 ~ /^_ZN9vibesynth/ { print $3; exit }')
    [ -n "$engine_sym" ] || { echo "strict: no engine symbol found in libvibesynth_engine.a"; return 1; }
    local kb out pad cmd sym a b
    local checked=""
    for kb in $STRICT_SHIFTS_KB; do
        [ "$kb" = 0 ] && continue
        out="strict-positions/vibesynth_cli_${kb}k"
        pad="strict-positions/pad_${kb}k"
        if [ ! -f "$out" ] || [ vibesynth_cli -nt "$out" ]; then
            # The GNU-stack note keeps the copy's stack non-executable, like the build's.
            printf '\t.section .text.unlikely.vibesynth_pad,"ax",@progbits\n\t.skip %d, 0xcc\n\t.section .note.GNU-stack,"",@progbits\n' \
                   $((kb * 1024)) > "$pad.s"
            as "$pad.s" -o "$pad.o" || { echo "strict: assembling $pad.s failed"; return 1; }
            cmd=$(printf '%s' "$link" \
                  | sed "s| -o vibesynth_cli | -o $out |; s|^: && \([^ ]*\) |: \&\& \1 $pad.o |")
            sh -c "$cmd" >"strict-positions/link_${kb}k.log" 2>&1 \
                || { echo "strict: linking $out failed (strict-positions/link_${kb}k.log)"; return 1; }
        fi
        for sym in main "$engine_sym" NSEEL_code_execute; do
            a=$(nm vibesynth_cli | awk -v s="$sym" '$3 == s { print $1; exit }')
            b=$(nm "$out" | awk -v s="$sym" '$3 == s { print $1; exit }')
            if [ -z "$a" ] || [ -z "$b" ] || [ $((16#$b - 16#$a)) -ne $((kb * 1024)) ]; then
                echo "strict: $out: $sym is at ${b:-?} (build: ${a:-?}), not moved by $((kb * 1024)) bytes"
                return 1
            fi
        done
        checked="$checked +$kb"
    done
    echo "code positions: +0 KB (the build) and${checked} KB, each shift checked with nm (main, $engine_sym, NSEEL_code_execute)"
}

# Inside the box: setarch -R must really turn randomisation off here.
box_check_norandom() {
    local m1 m2
    m1=$(setarch -R grep -m1 r-xp /proc/self/maps) || { echo "strict: setarch -R failed"; return 1; }
    m2=$(setarch -R grep -m1 r-xp /proc/self/maps)
    [ -n "$m1" ] && [ "$m1" = "$m2" ] && return 0
    echo "strict: setarch -R doesn't turn address randomisation off in this container"
    return 1
}

# Inside the box: one position in a fresh process, after the warm-up spin.
box_position() {
    local bin
    bin=$(position_cli "$1")
    taskset -c "$STRICT_CORE" timeout "$STRICT_WARMUP_S" sh -c 'while :; do :; done'
    exec setarch -R taskset -c "$STRICT_CORE" "$bin" --selftest --strict-cpu
}

# Selftest CPU lines in the given position logs: one line per preset with its
# best position (which gates) and its worst; [NOTE] when the worst is over
# budget or the best is within 5 % of it. A preset's figure at a position is
# its larger share of the voice or master budget there. Exit 1 if any preset
# is over budget at its best position or is missing from a log.
summarize_cpu() {
    awk '
        FNR == 1 { np++; p = FILENAME; sub(/.*selftest-at-/, "", p); sub(/k\.log$/, "", p); p = "+" p " KB" }
        / fits its CPU budget: voice / {
            f = $2; v = $8; mode = $10; vm = $12; m = $14; mm = $17
            gsub(/[(,]/, "", mode); gsub(/[),]/, "", vm); gsub(/[),]/, "", mm)
            # A line that does not parse (or an unmeasured voice of 0 ns) counts as over budget.
            if (v + 0 <= 0 || vm + 0 <= 0 || mm + 0 <= 0) s = 99
            else { s = v / vm; if (m / mm > s) s = m / mm }
            if (!(f in seen)) { order[++nf] = f; best[f] = s + 1; worst[f] = -1 }
            seen[f]++
            if (s < best[f]) { best[f] = s; bp[f] = p; bv[f] = v; bm[f] = m }
            if (s > worst[f]) { worst[f] = s; wp[f] = p; wv[f] = v; wm[f] = m }
            vmax[f] = vm; mmax[f] = mm; md[f] = mode
        }
        END {
            bad = 0
            for (i = 1; i <= nf; i++) {
                f = order[i]
                ok = best[f] <= 1 && seen[f] == np
                if (!ok) bad++
                printf "[%s] %s fits its CPU budget at its best of %d code positions: voice %s ns, master %s ns at %s (%s, max %s / %s); worst: voice %s ns, master %s ns at %s\n", \
                    ok ? "PASS" : "FAIL", f, np, bv[f], bm[f], bp[f], md[f], vmax[f], mmax[f], wv[f], wm[f], wp[f]
                if (seen[f] != np)
                    printf "[NOTE] %s: CPU measured at only %d of %d positions\n", f, seen[f], np
                if (worst[f] > 1)
                    printf "[NOTE] %s is over its budget at its worst position (%s): %.0f%% of it\n", f, wp[f], 100 * worst[f]
                if (best[f] >= 0.95)
                    printf "[NOTE] %s at its best position (%s) is within 5%% of its budget: %.0f%% of it\n", f, bp[f], 100 * best[f]
            }
            printf "-- CPU: %d presets, %d pass at their best position, %d fail\n", nf, nf - bad, bad
            exit (bad > 0 || nf == 0) ? 1 : 0
        }' "$@"
}

# The CPU budget gate (SPEC 6.4): the selftest at every code position, each
# in a fresh process on one core after a warm-up. Within a process the
# selftest still measures a preset over budget up to 3 times and keeps its
# lowest figures (timing noise); between processes only the position changes.
# Position +0 KB's output is shown as the plain selftest's; the other
# positions show only their failures.
step_strict_selftest() {
    [ -x "$CLI" ] || { echo "$CLI not built"; return 1; }
    run_in_box "$SELF" _build-positions || return 1
    run_in_box "$SELF" _check-norandom || return 1
    rm -f "$LOG_DIR"/selftest-at-*.log
    local kb log rc pass fail other n=0 bad=0 logs=()
    for kb in $STRICT_SHIFTS_KB; do n=$((n + 1)); done
    echo "== selftest (--selftest --strict-cpu) at $n code positions: setarch -R, core $STRICT_CORE, ${STRICT_WARMUP_S} s warm-up each"
    for kb in $STRICT_SHIFTS_KB; do
        log="$LOG_DIR/selftest-at-${kb}k.log"
        logs+=("$log")
        run_in_box "$SELF" _position "$kb" >"$log" 2>&1
        rc=$?
        pass=$(grep -c "^\[PASS\]" "$log")
        fail=$(grep -c "^\[FAIL\]" "$log")
        other=$(grep "^\[FAIL\]" "$log" | grep -v " fits its CPU budget: " \
                | grep -vE "^\[FAIL\] [0-9]+/[0-9]+ built-in presets pass$")
        if [ "$kb" = 0 ]; then
            grep -v "^\[PASS\]" "$log" | grep -v " fits its CPU budget: " \
                | grep -vE "^\[FAIL\] [0-9]+/[0-9]+ built-in presets pass$|^SELFTEST (PASS|FAIL)$"
        elif [ -n "$other" ]; then
            printf '%s\n' "$other" | sed "s/\$/ (at +$kb KB)/"
        fi
        if ! grep -qE "^SELFTEST (PASS|FAIL)$" "$log"; then
            echo "[FAIL] the selftest at +$kb KB didn't finish (exit $rc)"
            bad=1
        elif [ -n "$other" ]; then
            bad=1
        fi
        echo "-- +$kb KB: exit $rc, $pass passed, $fail failed (full log: $log)"
    done
    summarize_cpu "${logs[@]}" || bad=1
    [ "$bad" = 0 ] && echo "STRICT SELFTEST PASS" || echo "STRICT SELFTEST FAIL"
    return $bad
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
    # Internal steps of strict, run inside the box.
    _build-positions) box_build_positions || rc=1 ;;
    _check-norandom)  box_check_norandom || rc=1 ;;
    _position)        box_position "$2" ;;
    *) echo "usage: $0 [all|build|smoke|self|strict]"; exit 2 ;;
esac
exit $rc
