#!/usr/bin/env bash
# =============================================================================
# run_bench.sh — OpenArena seL4 memory benchmark runner
#
# What this script does:
#   1. Downloads the OpenArena 0.8.8 game data if not already present
#   2. Extracts only the baseoa/ pk3 files needed for the headless server
#   3. Copies the benchmark config (autoexec.cfg) into the data tree
#   4. Runs oa_bench with the requested number of frames and map
#
# Usage:
#   ./run_bench.sh [--frames N] [--map MAP] [--bots N] [--skill N]
#
# Options:
#   --frames N    Number of game frames to simulate (default: 1000)
#   --map   MAP   Map name to load               (default: q3dm6)
#   --bots  N     Number of bots to spawn        (default: 4)
#   --skill N     Bot skill level 1-5            (default: 3)
#
# Example:
#   ./run_bench.sh --frames 2000 --map q3dm1 --bots 6 --skill 4
# =============================================================================

set -euo pipefail

# ---------------------------------------------------------------------------
# Defaults
# ---------------------------------------------------------------------------
FRAMES=1000
MAP="oa_dm6"
BOTS=4
SKILL=3

# ---------------------------------------------------------------------------
# Parse arguments
# ---------------------------------------------------------------------------
while [[ $# -gt 0 ]]; do
    case "$1" in
        --frames) FRAMES="$2"; shift 2 ;;
        --map)    MAP="$2";    shift 2 ;;
        --bots)   BOTS="$2";  shift 2 ;;
        --skill)  SKILL="$2"; shift 2 ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DATA_DIR="$SCRIPT_DIR/gamedata"
BASEOA_DIR="$DATA_DIR/baseoa"
CONFIG_DIR="$SCRIPT_DIR/bench_config/baseoa"

# Detect the built binary (prefer arm64, fall back to x86_64)
BENCH_BIN=""
for b in \
    "$SCRIPT_DIR/build/release-darwin-arm/oa_bench.arm" \
    "$SCRIPT_DIR/build/release-darwin-arm64/oa_bench.arm64" \
    "$SCRIPT_DIR/build/release-darwin-x86_64/oa_bench.x86_64" \
    "$SCRIPT_DIR/build/release-linux-x86_64/oa_bench.x86_64" \
    "$SCRIPT_DIR/build/release-linux-arm64/oa_bench.arm64"; do
    if [[ -f "$b" ]]; then
        BENCH_BIN="$b"
        break
    fi
done

if [[ -z "$BENCH_BIN" ]]; then
    echo ""
    echo "ERROR: Could not find the oa_bench binary."
    echo "  Build it first with:"
    echo ""
    echo "    make memtest BUILD_CLIENT=0 BUILD_SERVER=0 \\"
    echo "        BUILD_GAME_SO=0 BUILD_GAME_QVM=0 \\"
    echo "        BUILD_BASEGAME=0 BUILD_MISSIONPACK=0 \\"
    echo "        BUILD_RENDERER_OPENGL2=0"
    echo ""
    exit 1
fi

echo ""
echo "=== OpenArena seL4 Memory Benchmark ==="
echo "  Binary  : $BENCH_BIN"
echo "  Map     : $MAP"
echo "  Frames  : $FRAMES"
echo "  Bots    : $BOTS (skill $SKILL)"
echo ""

# ---------------------------------------------------------------------------
# Step 1: Download game data if not already present
# ---------------------------------------------------------------------------
OA_ZIP="$SCRIPT_DIR/openarena-0.8.8.zip"
OA_URL="https://sourceforge.net/projects/oarena/files/openarena-0.8.8.zip/download"

if [[ ! -d "$BASEOA_DIR" ]] || [[ -z "$(ls -A "$BASEOA_DIR" 2>/dev/null)" ]]; then
    echo "--- Game data not found. Downloading OpenArena 0.8.8 (~300 MB) ---"
    echo ""

    if ! command -v curl &>/dev/null && ! command -v wget &>/dev/null; then
        echo "ERROR: Neither curl nor wget is available."
        echo "  Please manually download:"
        echo "    $OA_URL"
        echo "  and extract the baseoa/ folder to: $BASEOA_DIR"
        exit 1
    fi

    mkdir -p "$DATA_DIR"

    if [[ ! -f "$OA_ZIP" ]]; then
        echo "Downloading $OA_URL ..."
        if command -v curl &>/dev/null; then
            curl -L --progress-bar -o "$OA_ZIP" "$OA_URL"
        else
            wget -O "$OA_ZIP" "$OA_URL"
        fi
        echo "Download complete."
    else
        echo "Zip already present at $OA_ZIP, skipping download."
    fi

    echo ""
    echo "--- Extracting baseoa/ pk3 files ---"
    mkdir -p "$BASEOA_DIR"

    # Extract only the baseoa/ subtree (skip executables, docs, etc.)
    if command -v unzip &>/dev/null; then
        unzip -q -o "$OA_ZIP" "openarena-0.8.8/baseoa/*" -d "$DATA_DIR/_extract"
        # The zip puts files under openarena-0.8.8/baseoa/; move them up
        mv "$DATA_DIR/_extract/openarena-0.8.8/baseoa/"* "$BASEOA_DIR/"
        rm -rf "$DATA_DIR/_extract"
    else
        echo "ERROR: unzip not found. Install it with: brew install unzip"
        exit 1
    fi

    echo "Extraction complete."
    echo ""
else
    echo "--- Game data already present at $BASEOA_DIR ---"
    echo ""
fi

# ---------------------------------------------------------------------------
# Step 2: Install the benchmark config into the data tree
# ---------------------------------------------------------------------------
# autoexec.cfg is loaded automatically by the engine after default.cfg.
# We place it directly in baseoa/ so it sits alongside the pk3 files.
echo "--- Installing benchmark config ---"
cp "$CONFIG_DIR/autoexec.cfg" "$BASEOA_DIR/autoexec.cfg"

# Patch the skill level from command-line into the config copy
sed -i.bak "s/^set g_spSkill.*/set g_spSkill       $SKILL/" \
    "$BASEOA_DIR/autoexec.cfg"
sed -i.bak "s/^set bot_minplayers.*/set bot_minplayers  $BOTS/" \
    "$BASEOA_DIR/autoexec.cfg"
rm -f "$BASEOA_DIR/autoexec.cfg.bak"

echo "  Config  : $BASEOA_DIR/autoexec.cfg"
echo ""

# ---------------------------------------------------------------------------
# Step 3: Build the addbot commands
# ---------------------------------------------------------------------------
# OpenArena bots in roughly increasing difficulty order
BOT_ROSTER=("Gargoyle" "Grism" "Kyonshi" "Major" "Merman" "Sergei" "Sarge" "Grunt")
BOT_CMDS=""
for (( i=0; i<BOTS && i<${#BOT_ROSTER[@]}; i++ )); do
    BOT_CMDS="$BOT_CMDS +addbot ${BOT_ROSTER[$i]} $SKILL"
done

# ---------------------------------------------------------------------------
# Step 4: Run the benchmark
# ---------------------------------------------------------------------------
echo "--- Launching benchmark ---"
echo ""

CMD="\"$BENCH_BIN\" \
    +set dedicated 1 \
    +set fs_basepath \"$DATA_DIR\" \
    +set fs_homepath \"$DATA_DIR\" \
    +set vm_game 0 \
    +set g_gametype 0 \
    +set bot_enable 1 \
    +set bot_nochat 1 \
    +set g_forcerespawn 1 \
    +set fraglimit 0 \
    +set timelimit 0 \
    +map $MAP \
    $BOT_CMDS \
    --bench-frames $FRAMES"

echo "Command:"
echo "  $CMD"
echo ""

eval "$CMD"
