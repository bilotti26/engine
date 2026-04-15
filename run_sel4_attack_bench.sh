#!/usr/bin/env bash
# =============================================================================
# run_sel4_attack_bench.sh  -  Build and run the OA seL4 read-only enforcement
#                              test (attacker PD attempts write to snapshot page)
#
# This script is identical to run_sel4_bench.sh except:
#   - Uses a separate build directory (sel4/build_attack) to avoid clobbering
#     the regular benchmark build.
#   - Passes ATTACK_BUILD=1 and SYSTEM_FILE=sel4/bench_attack.system to make,
#     which adds the attacker PD and compiles with -DMEMTEST_ATTACK.
#   - Defaults to --frames 200 (the fault fires on frame 1; extra frames let
#     the detectors keep running after the attacker is terminated).
#   - Post-run grep prints ENFORCEMENT CONFIRMED or ENFORCEMENT FAILED.
#
# Expected output:
#   attacker: ready — will attempt write on first frame notify
#   attacker: attempting write to read-only snapshot page...
#   <seL4 VM fault / capability violation message from kernel>
#   (no "BUG: attacker write succeeded" line)
#
# Usage:
#   ./run_sel4_attack_bench.sh [--frames N] [--clean]
# =============================================================================

set -euo pipefail

# ---------------------------------------------------------------------------
# Defaults
# ---------------------------------------------------------------------------
FRAMES=200
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
BUILD_DIR="$SEL4_DIR/build_attack"
DATA_DIR="$SEL4_DIR/data"
GAMEDATA_DIR="$SCRIPT_DIR/gamedata"

SDK_VERSION="1.4.1"
SDK_ARCHIVE="microkit-sdk-${SDK_VERSION}-linux-x86-64.tar.gz"
SDK_URL="https://github.com/seL4/microkit/releases/download/${SDK_VERSION}/${SDK_ARCHIVE}"
SDK_CACHE="$SCRIPT_DIR/.microkit-sdk"
MICROKIT_SDK="$SDK_CACHE/microkit-sdk-${SDK_VERSION}"

SYSTEM_FILE="$SEL4_DIR/bench_attack.system"

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
step() { echo ""; echo "--- $* ---"; echo ""; }
die()  { echo "ERROR: $*" >&2; exit 1; }

# ---------------------------------------------------------------------------
# Step 0: Handle --clean
# ---------------------------------------------------------------------------
if [[ $CLEAN -eq 1 ]]; then
    step "Cleaning attack build artefacts"
    rm -rf "$BUILD_DIR"
    echo "Done."
fi

# ---------------------------------------------------------------------------
# Step 1: Download seL4 Microkit SDK (reuse cache from regular bench)
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

BOARD="qemu_virt_aarch64"
if [[ ! -d "$MICROKIT_SDK/board/$BOARD" ]]; then
    die "Board '$BOARD' not found in SDK at $MICROKIT_SDK/board/."
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
# Step 4: Ensure game data is present (reuse from regular bench)
# ---------------------------------------------------------------------------
BASEOA_DIR="$GAMEDATA_DIR/baseoa"
if [[ ! -d "$BASEOA_DIR" ]] || [[ -z "$(ls -A "$BASEOA_DIR"/*.pk3 2>/dev/null)" ]]; then
    step "Fetching OpenArena game data (reusing run_bench.sh logic)"
    "$SCRIPT_DIR/run_bench.sh" --frames 1 2>&1 | head -40 || true
    if [[ ! -f "$BASEOA_DIR/pak0.pk3" ]]; then
        die "Game data not found at $BASEOA_DIR. Run ./run_bench.sh first."
    fi
fi
echo "Game data present at $BASEOA_DIR"

# ---------------------------------------------------------------------------
# Step 5: Build minimal CPIO game data package (reuse from regular bench)
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
# Step 6: Build seL4 attack image
# ---------------------------------------------------------------------------
step "Building seL4 attack image (ATTACK_BUILD=1)"

mkdir -p "$BUILD_DIR"

make -C "$SEL4_DIR" \
    MICROKIT_SDK="$MICROKIT_SDK" \
    DATA_DIR="$DATA_DIR" \
    BUILD_DIR="$BUILD_DIR" \
    BENCH_FRAMES="$FRAMES" \
    MICROKIT_BOARD="$BOARD" \
    MICROKIT_CONFIG=debug \
    ATTACK_BUILD=1 \
    SYSTEM_FILE="$SYSTEM_FILE"

IMAGE="$BUILD_DIR/loader.img"
if [[ ! -f "$IMAGE" ]]; then
    die "Build failed: $IMAGE not found."
fi
echo "Image: $IMAGE ($(du -h "$IMAGE" | cut -f1))"

# ---------------------------------------------------------------------------
# Step 7: Launch QEMU
# ---------------------------------------------------------------------------
step "Launching QEMU aarch64 (enforcement test)"
echo "Platform : $BOARD"
echo "Frames   : $FRAMES"
echo "Image    : $IMAGE"
echo ""
echo "Expected: seL4 VM fault after attacker attempts write on frame 1."
echo ""

QEMU_OUT="$BUILD_DIR/attack_output.txt"
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

tail -f "$QEMU_OUT" &
TAIL_PID=$!

TIMEOUT_SEC=$(( FRAMES * 2 + 120 ))
ELAPSED=0
DONE=0
while [[ $ELAPSED -lt $TIMEOUT_SEC ]]; do
    sleep 2
    ELAPSED=$(( ELAPSED + 2 ))
    if grep -qa "BENCH: done. Halting." "$QEMU_OUT" 2>/dev/null; then
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
echo "Output saved to: $QEMU_OUT"
echo ""

# ---------------------------------------------------------------------------
# Step 8: Verdict
# ---------------------------------------------------------------------------
echo "=== Enforcement verdict ==="
if grep -qa "BUG: attacker write succeeded" "$QEMU_OUT" 2>/dev/null; then
    echo "ENFORCEMENT FAILED -- attacker write was not blocked by the MMU."
    echo "Check bench_attack.system: snapshot mapping for attacker must use perms=\"r\"."
    exit 1
elif grep -qa "attacker: attempting write" "$QEMU_OUT" 2>/dev/null; then
    echo "ENFORCEMENT CONFIRMED -- write attempt triggered a fault; attacker was terminated."
    exit 0
else
    echo "WARNING: attacker PD did not fire (check channel 3 wiring or build flags)."
    grep -aE "attacker|BENCH:|fault|violation" "$QEMU_OUT" | head -20 || true
    exit 2
fi
