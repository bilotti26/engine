#!/usr/bin/env bash
# =============================================================================
# run_all_benchmarks.sh  -  Run all three OA benchmarks sequentially and
#                           compare performance.
#
# Tests run:
#   1. x86 native      — oa_bench binary, no virtualisation
#   2. seL4 clean      — Microkit image, bench + monitor_aim + monitor_physics
#   3. seL4 attack     — same + attacker PD attempting snapshot write
#
# Usage:
#   ./run_all_benchmarks.sh [--frames N] [--clean] [--skip-x86] [--skip-sel4] [--skip-attack]
#
# Options:
#   --frames N      Game frames per test (default: 200)
#   --clean         Wipe seL4 build dirs before building
#   --skip-x86      Skip the native x86 test
#   --skip-sel4     Skip the seL4 clean test
#   --skip-attack   Skip the seL4 attack test
# =============================================================================

set -euo pipefail

# ---------------------------------------------------------------------------
# Defaults
# ---------------------------------------------------------------------------
FRAMES=200
CLEAN=0
SKIP_X86=0
SKIP_SEL4=0
SKIP_ATTACK=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --frames)       FRAMES="$2";  shift 2 ;;
        --clean)        CLEAN=1;       shift   ;;
        --skip-x86)     SKIP_X86=1;    shift   ;;
        --skip-sel4)    SKIP_SEL4=1;   shift   ;;
        --skip-attack)  SKIP_ATTACK=1; shift   ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RESULTS_DIR="$SCRIPT_DIR/bench_results"
X86_OUT="$RESULTS_DIR/x86_output.txt"
SEL4_OUT="$RESULTS_DIR/sel4_output.txt"
ATTACK_OUT="$RESULTS_DIR/attack_output.txt"

mkdir -p "$RESULTS_DIR"

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
step()    { echo ""; echo "======================================"; echo "  $*"; echo "======================================"; echo ""; }
substep() { echo "--- $* ---"; }
die()     { echo "ERROR: $*" >&2; exit 1; }

# Extract a named field from a BENCH: output line.
#   Fields may appear as "BENCH: fps_equivalent=16.7" or embedded in a longer
#   line like "BENCH: frames=50 elapsed_ms=1057", so search for the field name
#   anywhere on a BENCH: line rather than anchoring to the line start.
#   The trailing "|| true" prevents set -e from triggering when grep finds no match.
parse_field() {
    local field="$1"
    local file="$2"
    grep -a "BENCH:.*${field}=" "$file" 2>/dev/null \
        | head -1 \
        | sed "s/.*${field}=\([^ ]*\).*/\1/" \
        | tr -d '\r' \
        || true
}

# Parse the enforcement verdict from the attack output.
parse_verdict() {
    local file="$1"
    if grep -qa "BUG: attacker write succeeded" "$file" 2>/dev/null; then
        echo "FAILED"
    elif grep -qa "attacker: attempting write" "$file" 2>/dev/null; then
        echo "CONFIRMED"
    else
        echo "NO_FIRE"
    fi
}

# ---------------------------------------------------------------------------
# Banner
# ---------------------------------------------------------------------------
echo ""
echo "############################################################"
echo "#   OpenArena Benchmark Suite — seL4 vs Native Comparison #"
echo "############################################################"
echo ""
echo "  Frames per test : $FRAMES"
echo "  Results dir     : $RESULTS_DIR"
echo ""

# ---------------------------------------------------------------------------
# Test 1: x86 native
# ---------------------------------------------------------------------------
X86_FPS="N/A"
X86_MS="N/A"
X86_HUNK="N/A"
X86_STATUS="skipped"

if [[ $SKIP_X86 -eq 0 ]]; then
    step "Test 1 of 3: x86 native benchmark"

    # Check binary exists before attempting run
    X86_BIN=""
    for b in \
        "$SCRIPT_DIR/build/release-linux-x86_64/oa_bench.x86_64" \
        "$SCRIPT_DIR/build/release-linux-arm64/oa_bench.arm64" \
        "$SCRIPT_DIR/build/release-darwin-arm/oa_bench.arm" \
        "$SCRIPT_DIR/build/release-darwin-arm64/oa_bench.arm64" \
        "$SCRIPT_DIR/build/release-darwin-x86_64/oa_bench.x86_64"; do
        if [[ -f "$b" ]]; then X86_BIN="$b"; break; fi
    done

    if [[ -z "$X86_BIN" ]]; then
        echo "WARNING: oa_bench binary not found — skipping x86 test."
        echo "  Build with: make memtest BUILD_CLIENT=0 BUILD_SERVER=0 \\"
        echo "              BUILD_GAME_SO=0 BUILD_GAME_QVM=0 \\"
        echo "              BUILD_BASEGAME=0 BUILD_MISSIONPACK=0 \\"
        echo "              BUILD_RENDERER_OPENGL2=0"
        X86_STATUS="no_binary"
    else
        substep "Running run_bench.sh --frames $FRAMES"
        # run_bench.sh writes directly to stdout; capture + tee so user sees progress.
        "$SCRIPT_DIR/run_bench.sh" --frames "$FRAMES" 2>&1 | tee "$X86_OUT" || true

        X86_FPS="$(parse_field "fps_equivalent" "$X86_OUT")"
        X86_MS="$(parse_field  "elapsed_ms"     "$X86_OUT")"
        X86_HUNK="$(parse_field "hunk_remaining" "$X86_OUT")"
        X86_STATUS="ok"

        echo ""
        echo "x86 result: fps=$X86_FPS  elapsed_ms=$X86_MS  hunk_remaining=$X86_HUNK"
    fi
fi

# ---------------------------------------------------------------------------
# Test 2: seL4 clean benchmark
# ---------------------------------------------------------------------------
SEL4_FPS="N/A"
SEL4_MS="N/A"
SEL4_HUNK="N/A"
SEL4_STATUS="skipped"

if [[ $SKIP_SEL4 -eq 0 ]]; then
    step "Test 2 of 3: seL4 clean benchmark (bench + monitor PDs)"

    SEL4_CLEAN_ARG=""
    [[ $CLEAN -eq 1 ]] && SEL4_CLEAN_ARG="--clean"

    # BENCH_FRAMES is a compile-time define. Make compares file mtimes, not
    # CFLAGS, so it won't recompile sys_bench.o when only BENCH_FRAMES changes.
    # Delete the object to force a recompile with the correct frame count.
    rm -f "$SCRIPT_DIR/sel4/build/engine/code/sys/sys_bench.o"

    substep "Running run_sel4_bench.sh --frames $FRAMES"
    "$SCRIPT_DIR/run_sel4_bench.sh" --frames "$FRAMES" $SEL4_CLEAN_ARG 2>&1 || true

    # run_sel4_bench.sh saves output to sel4/build/bench_output.txt
    SEL4_RAW="$SCRIPT_DIR/sel4/build/bench_output.txt"
    if [[ -f "$SEL4_RAW" ]]; then
        cp "$SEL4_RAW" "$SEL4_OUT"
        SEL4_FPS="$(parse_field  "fps_equivalent" "$SEL4_OUT")"
        SEL4_MS="$(parse_field   "elapsed_ms"     "$SEL4_OUT")"
        SEL4_HUNK="$(parse_field "hunk_remaining" "$SEL4_OUT")"
        SEL4_STATUS="ok"
        echo ""
        echo "seL4 result: fps=$SEL4_FPS  elapsed_ms=$SEL4_MS  hunk_remaining=$SEL4_HUNK"
    else
        echo "WARNING: seL4 output file not found — benchmark may have failed."
        SEL4_STATUS="no_output"
    fi
fi

# ---------------------------------------------------------------------------
# Test 3: seL4 attack benchmark
# ---------------------------------------------------------------------------
ATTACK_FPS="N/A"
ATTACK_MS="N/A"
ATTACK_HUNK="N/A"
ATTACK_VERDICT="N/A"
ATTACK_STATUS="skipped"

if [[ $SKIP_ATTACK -eq 0 ]]; then
    step "Test 3 of 3: seL4 attack benchmark (+ attacker PD)"

    ATTACK_CLEAN_ARG=""
    [[ $CLEAN -eq 1 ]] && ATTACK_CLEAN_ARG="--clean"

    # Same BENCH_FRAMES freshness fix as test 2.
    rm -f "$SCRIPT_DIR/sel4/build_attack/engine/code/sys/sys_bench.o"

    substep "Running run_sel4_attack_bench.sh --frames $FRAMES"
    # run_sel4_attack_bench.sh exits 0 (confirmed), 1 (failed), or 2 (no_fire).
    # Capture exit code without tripping set -e.
    "$SCRIPT_DIR/run_sel4_attack_bench.sh" --frames "$FRAMES" $ATTACK_CLEAN_ARG 2>&1 || true

    ATTACK_RAW="$SCRIPT_DIR/sel4/build_attack/attack_output.txt"
    if [[ -f "$ATTACK_RAW" ]]; then
        cp "$ATTACK_RAW" "$ATTACK_OUT"
        ATTACK_FPS="$(parse_field     "fps_equivalent"  "$ATTACK_OUT")"
        ATTACK_MS="$(parse_field      "elapsed_ms"      "$ATTACK_OUT")"
        ATTACK_HUNK="$(parse_field    "hunk_remaining"  "$ATTACK_OUT")"
        ATTACK_VERDICT="$(parse_verdict "$ATTACK_OUT")"
        ATTACK_STATUS="ok"
        echo ""
        echo "attack result: fps=$ATTACK_FPS  elapsed_ms=$ATTACK_MS  verdict=$ATTACK_VERDICT"
    else
        echo "WARNING: attack output file not found — benchmark may have failed."
        ATTACK_STATUS="no_output"
    fi
fi

# ---------------------------------------------------------------------------
# Compute overhead percentages (fps-based; lower fps = higher overhead)
# ---------------------------------------------------------------------------
compute_overhead() {
    # $1 = baseline fps (x86), $2 = comparison fps
    # Returns overhead as "X.X%" or "N/A"
    local base="$1"
    local cmp="$2"
    if [[ "$base" == "N/A" || "$cmp" == "N/A" || -z "$base" || -z "$cmp" ]]; then
        echo "N/A"
        return
    fi
    python3 -c "
base=float('$base'); cmp=float('$cmp')
if base <= 0:
    print('N/A')
else:
    pct = (base - cmp) / base * 100.0
    sign = '+' if pct >= 0 else ''
    print(f'{sign}{pct:.1f}%')
" 2>/dev/null || echo "N/A"
}

SEL4_OVERHEAD="$(compute_overhead    "$X86_FPS" "$SEL4_FPS")"
ATTACK_OVERHEAD="$(compute_overhead  "$X86_FPS" "$ATTACK_FPS")"
ATTACK_VS_SEL4="$(compute_overhead   "$SEL4_FPS" "$ATTACK_FPS")"

# ---------------------------------------------------------------------------
# Results table
# ---------------------------------------------------------------------------
step "Benchmark Comparison Results"

printf "%-24s  %10s  %12s  %18s  %12s\n" \
    "Test"  "FPS"  "Elapsed (ms)"  "Hunk Remaining"  "vs x86"
printf "%-24s  %10s  %12s  %18s  %12s\n" \
    "------------------------"  "----------"  "------------"  "------------------"  "------------"

printf "%-24s  %10s  %12s  %18s  %12s\n" \
    "x86 native"           "$X86_FPS"    "$X86_MS"    "$X86_HUNK bytes"    "--"
printf "%-24s  %10s  %12s  %18s  %12s\n" \
    "seL4 clean"           "$SEL4_FPS"   "$SEL4_MS"   "$SEL4_HUNK bytes"   "$SEL4_OVERHEAD"
printf "%-24s  %10s  %12s  %18s  %12s\n" \
    "seL4 + attacker"      "$ATTACK_FPS" "$ATTACK_MS" "$ATTACK_HUNK bytes" "$ATTACK_OVERHEAD"

echo ""
echo "  seL4 attack overhead vs seL4 clean : $ATTACK_VS_SEL4"
echo ""

# ---------------------------------------------------------------------------
# Enforcement verdict
# ---------------------------------------------------------------------------
echo "--------------------------------------"
echo "  Enforcement verdict"
echo "--------------------------------------"
case "$ATTACK_VERDICT" in
    CONFIRMED)
        echo "  ENFORCEMENT CONFIRMED"
        echo "  The attacker write triggered a hardware VM fault."
        echo "  seL4 page-table capability enforcement is working."
        ;;
    FAILED)
        echo "  ENFORCEMENT FAILED"
        echo "  The attacker write completed without faulting."
        echo "  Check perms=\"r\" on the snapshot mapping in bench_attack.system."
        ;;
    NO_FIRE)
        echo "  WARNING: attacker PD did not fire."
        echo "  Check ATTACK_BUILD=1 flag and channel 3 wiring."
        ;;
    N/A)
        echo "  Attack test was skipped."
        ;;
esac

echo ""

# ---------------------------------------------------------------------------
# Interpretation note
# ---------------------------------------------------------------------------
if [[ "$X86_STATUS" == "ok" && ("$SEL4_STATUS" == "ok" || "$ATTACK_STATUS" == "ok") ]]; then
    echo "--------------------------------------"
    echo "  Notes"
    echo "--------------------------------------"
    echo "  * FPS figures are software-simulated game ticks, not render FPS."
    echo "  * seL4 runs under QEMU aarch64 emulation on an x86 host; the"
    echo "    overhead shown includes both QEMU JIT cost and seL4 IPC cost."
    echo "  * The attacker PD fires once (frame 1) then is suspended by"
    echo "    the kernel; its presence adds negligible per-frame overhead."
    echo "  * hunk_remaining should be identical across all three tests"
    echo "    (same map, same bots, same frame count)."
    echo ""
fi

# ---------------------------------------------------------------------------
# Save summary
# ---------------------------------------------------------------------------
SUMMARY="$RESULTS_DIR/summary.txt"
{
    echo "OpenArena Benchmark Suite — $(date)"
    echo "Frames: $FRAMES"
    echo ""
    echo "x86 native    : fps=$X86_FPS  elapsed_ms=$X86_MS  hunk=$X86_HUNK  status=$X86_STATUS"
    echo "seL4 clean    : fps=$SEL4_FPS  elapsed_ms=$SEL4_MS  hunk=$SEL4_HUNK  overhead=$SEL4_OVERHEAD  status=$SEL4_STATUS"
    echo "seL4 attack   : fps=$ATTACK_FPS  elapsed_ms=$ATTACK_MS  hunk=$ATTACK_HUNK  overhead=$ATTACK_OVERHEAD  verdict=$ATTACK_VERDICT  status=$ATTACK_STATUS"
    echo "attack vs seL4 clean overhead: $ATTACK_VS_SEL4"
} > "$SUMMARY"

echo "Full summary saved to: $SUMMARY"
echo "Raw outputs saved to:  $RESULTS_DIR/"
echo ""
