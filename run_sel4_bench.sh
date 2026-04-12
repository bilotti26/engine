#!/usr/bin/env bash
# =============================================================================
# run_sel4_bench.sh  -  Build and run the OA headless benchmark on seL4
#
# What this script does:
#   1. Downloads the seL4 Microkit SDK if not cached
#   2. Installs the aarch64 cross-compiler if missing
#   3. Ensures OpenArena game data is present (runs run_bench.sh data setup)
#   4. Extracts needed game files and builds a minimal CPIO archive
#   5. Compiles the benchmark as a seL4 Microkit protection domain
#   6. Launches QEMU aarch64 with the resulting image
#
# Usage:
#   ./run_sel4_bench.sh [--frames N] [--clean]
#
# Options:
#   --frames N   Number of game frames to simulate (default: 1000)
#   --clean      Remove build artefacts before building
# =============================================================================

set -euo pipefail

# ---------------------------------------------------------------------------
# Defaults
# ---------------------------------------------------------------------------
FRAMES=1000
CLEAN=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --frames) FRAMES="$2"; shift 2 ;;
        --clean)  CLEAN=1;     shift   ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SEL4_DIR="$SCRIPT_DIR/sel4"
BUILD_DIR="$SEL4_DIR/build"
DATA_DIR="$SEL4_DIR/data"
GAMEDATA_DIR="$SCRIPT_DIR/gamedata"

SDK_VERSION="1.4.1"
SDK_ARCHIVE="microkit-sdk-${SDK_VERSION}-linux-x86-64.tar.gz"
SDK_URL="https://github.com/seL4/microkit/releases/download/${SDK_VERSION}/${SDK_ARCHIVE}"
SDK_CACHE="$SCRIPT_DIR/.microkit-sdk"
MICROKIT_SDK="$SDK_CACHE/microkit-sdk-${SDK_VERSION}"

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
step() { echo ""; echo "--- $* ---"; echo ""; }
die()  { echo "ERROR: $*" >&2; exit 1; }

# ---------------------------------------------------------------------------
# Step 0: Handle --clean
# ---------------------------------------------------------------------------
if [[ $CLEAN -eq 1 ]]; then
    step "Cleaning build artefacts"
    rm -rf "$BUILD_DIR" "$DATA_DIR"
    echo "Done."
fi

# ---------------------------------------------------------------------------
# Step 1: Download seL4 Microkit SDK
# ---------------------------------------------------------------------------
if [[ ! -f "$MICROKIT_SDK/bin/microkit" ]]; then
    step "Downloading seL4 Microkit SDK ${SDK_VERSION}"
    mkdir -p "$SDK_CACHE"
    TMP_ARCHIVE="$SDK_CACHE/$SDK_ARCHIVE"
    if [[ ! -f "$TMP_ARCHIVE" ]]; then
        echo "Fetching $SDK_URL ..."
        curl -L --progress-bar -o "$TMP_ARCHIVE" "$SDK_URL"
    else
        echo "Cached archive found: $TMP_ARCHIVE"
    fi
    echo "Extracting ..."
    tar -xf "$TMP_ARCHIVE" -C "$SDK_CACHE"
    echo "SDK ready at $MICROKIT_SDK"
else
    echo "seL4 Microkit SDK already present."
fi

# Verify the board we target exists
BOARD="qemu_virt_aarch64"
if [[ ! -d "$MICROKIT_SDK/board/$BOARD" ]]; then
    die "Board '$BOARD' not found in SDK at $MICROKIT_SDK/board/. Available: $(ls $MICROKIT_SDK/board/)"
fi

# ---------------------------------------------------------------------------
# Step 2: Ensure cross-compiler is installed
# ---------------------------------------------------------------------------
if ! command -v aarch64-linux-gnu-gcc &>/dev/null && \
   ! command -v aarch64-none-elf-gcc  &>/dev/null; then
    step "Installing aarch64 cross-compiler"
    sudo apt-get install -y gcc-aarch64-linux-gnu
fi

if command -v aarch64-none-elf-gcc &>/dev/null; then
    CROSS="aarch64-none-elf"
elif command -v aarch64-linux-gnu-gcc &>/dev/null; then
    CROSS="aarch64-linux-gnu"
else
    die "No aarch64 cross-compiler found. Install gcc-aarch64-linux-gnu."
fi
echo "Cross-compiler: ${CROSS}-gcc ($(${CROSS}-gcc --version | head -1))"

# ---------------------------------------------------------------------------
# Step 3: Ensure QEMU aarch64 is installed
# ---------------------------------------------------------------------------
if ! command -v qemu-system-aarch64 &>/dev/null; then
    step "Installing qemu-system-aarch64"
    sudo apt-get install -y qemu-system-arm
fi

# ---------------------------------------------------------------------------
# Step 4: Ensure game data is present
# ---------------------------------------------------------------------------
BASEOA_DIR="$GAMEDATA_DIR/baseoa"
if [[ ! -d "$BASEOA_DIR" ]] || [[ -z "$(ls -A "$BASEOA_DIR"/*.pk3 2>/dev/null)" ]]; then
    step "Fetching OpenArena game data (reusing run_bench.sh logic)"
    # Run run_bench.sh with 0 frames just to download + extract the pk3s
    # We do it this way to avoid duplicating the download/extract logic.
    "$SCRIPT_DIR/run_bench.sh" --frames 1 2>&1 | head -40 || true
    # If still missing, fall back to direct download
    if [[ ! -f "$BASEOA_DIR/pak0.pk3" ]]; then
        die "Game data not found at $BASEOA_DIR. Run ./run_bench.sh first."
    fi
fi
echo "Game data present at $BASEOA_DIR"

# ---------------------------------------------------------------------------
# Step 5: Build minimal CPIO game data package
# ---------------------------------------------------------------------------
if [[ ! -f "$DATA_DIR/gamedata.cpio" ]] || [[ "$DATA_DIR/gamedata.cpio" -ot "$GAMEDATA_DIR/baseoa/pak6-patch088.pk3" ]]; then
    step "Building minimal seL4 game data (CPIO)"
    mkdir -p "$DATA_DIR"
    python3 "$SCRIPT_DIR/tools/mk_sel4_data.py" "$GAMEDATA_DIR" "$DATA_DIR"
else
    echo "CPIO data up to date."
fi

ls -lh "$DATA_DIR/gamedata.cpio"

# ---------------------------------------------------------------------------
# Step 6: Build seL4 image
# ---------------------------------------------------------------------------
step "Building seL4 benchmark image"

mkdir -p "$BUILD_DIR"

make -C "$SEL4_DIR" \
    MICROKIT_SDK="$MICROKIT_SDK" \
    DATA_DIR="$DATA_DIR" \
    BUILD_DIR="$BUILD_DIR" \
    BENCH_FRAMES="$FRAMES" \
    MICROKIT_BOARD="$BOARD" \
    MICROKIT_CONFIG=debug

IMAGE="$BUILD_DIR/loader.img"
if [[ ! -f "$IMAGE" ]]; then
    die "Build failed: $IMAGE not found."
fi
echo "Image: $IMAGE ($(du -h "$IMAGE" | cut -f1))"

# ---------------------------------------------------------------------------
# Step 7: Launch QEMU
# ---------------------------------------------------------------------------
step "Launching QEMU aarch64"
echo "Platform : $BOARD"
echo "Frames   : $FRAMES"
echo "Image    : $IMAGE"
echo ""

# Memory: 2 GB.
# The seL4 kernel (Microkit SDK, qemu_virt_aarch64) occupies physical 0x60000000+.
# avail_p_regs = [0x60000000, 0xc0000000). With -m 2G, QEMU RAM covers
# 0x40000000 to 0xbfffffff, which includes the full avail range.
#
# IMPORTANT: loader.img must be loaded at physical 0x70000000 (not the default
# QEMU kernel load address of 0x40080000). The Microkit loader binary was compiled
# for 0x70000000 and uses absolute addresses for its data structures. Use
# -device loader (not -kernel) to place it at the correct physical address.
#
# The 192 MB engine heap is at physical 0x80000000, mapped with 2 MB large pages.
#
# QEMU does not exit automatically when the benchmark finishes (the engine spins
# in Sys_Quit waiting for a PSCI call seL4 cannot forward).  We run QEMU in the
# background, stream its output to a file + stdout, and kill it the moment the
# benchmark sentinel line "BENCH: done. Halting." appears.

QEMU_OUT="$BUILD_DIR/bench_output.txt"
> "$QEMU_OUT"

qemu-system-aarch64 \
    -machine virt,virtualization=on \
    -cpu cortex-a53 \
    -m 2G \
    -nographic \
    -device loader,file="$IMAGE",addr=0x70000000,cpu-num=0 \
    >"$QEMU_OUT" 2>&1 &
QEMU_PID=$!

echo "QEMU PID: $QEMU_PID"
echo "Waiting for benchmark to finish (streaming output)..."
echo ""

# Stream output to stdout while watching for completion sentinel
tail -f "$QEMU_OUT" &
TAIL_PID=$!

TIMEOUT_SEC=$(( FRAMES * 2 + 120 ))   # generous: 2s/frame + 120s startup
ELAPSED=0
DONE=0
while [[ $ELAPSED -lt $TIMEOUT_SEC ]]; do
    sleep 2
    ELAPSED=$(( ELAPSED + 2 ))
    if grep -q "BENCH: done. Halting." "$QEMU_OUT" 2>/dev/null; then
        DONE=1
        break
    fi
    if ! kill -0 "$QEMU_PID" 2>/dev/null; then
        break
    fi
done

kill "$TAIL_PID" 2>/dev/null || true
kill "$QEMU_PID" 2>/dev/null || true
wait "$QEMU_PID" 2>/dev/null || true

echo ""
if [[ $DONE -eq 1 ]]; then
    echo "Benchmark completed successfully."
else
    echo "WARNING: benchmark timed out after ${TIMEOUT_SEC}s."
fi
echo "Output saved to: $QEMU_OUT"
echo ""
grep -E "BENCH:|fps_equivalent|hunk_remaining|Results" "$QEMU_OUT" | tail -10
