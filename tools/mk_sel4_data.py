#!/usr/bin/env python3
"""
mk_sel4_data.py - Build a minimal pk3 + CPIO for the seL4 benchmark.

Extracts only the files the headless server actually needs from the OpenArena
pak archives, repacks them into a single small minipak.pk3, then wraps the
whole baseoa/ directory into a CPIO archive for embedding in the seL4 binary.

Usage:
    python3 tools/mk_sel4_data.py <gamedata_dir> <output_dir>

Output:
    <output_dir>/gamedata.cpio   -- CPIO archive (newc format)
"""

import sys, os, zipfile, io, struct

# ---------------------------------------------------------------------------
# Files to extract (highest-priority pak wins if file appears in multiple)
# ---------------------------------------------------------------------------
NEEDED = [
    # VM bytecode -- critical
    "vm/qagame.qvm",
    # Map geometry and bot navigation -- critical
    "maps/oa_dm6.bsp",
    "maps/oa_dm6.aas",
    # Engine config -- critical (engine aborts if missing)
    "default.cfg",
    # Bot system config
    "scripts/bots.txt",
    "scripts/arenas.txt",
    # Bot behaviour scripts (base engine files)
    "botfiles/weapons.c",
    "botfiles/items.c",
    "botfiles/syn.c",
    "botfiles/rnd.c",
    "botfiles/match.c",
    "botfiles/fw_items.c",
    "botfiles/fw_weap.c",
    # Bot script header files (included by bot personality scripts)
    "botfiles/chars.h",
    "botfiles/inv.h",
    "botfiles/match.h",
    "botfiles/syn.h",
    "botfiles/teamplay.h",
    # Fuzzy logic modules included by bot *_i.c and *_w.c scripts
    "botfiles/fuzi.c",
    "botfiles/fuzw.c",
]

# All individual bot personality files
BOT_NAMES = [
    "angelyss","arachna","ayumi","beret","dark","default",
    "gargoyle","grism","grunt","jenna","kyonshi","liz","major",
    "merman","neko","penguin","rai","sarge","sergei","skelebot",
    "smarine","sorceress","tony","assassin",
]
for bot in BOT_NAMES:
    for suffix in ["_c.c","_i.c","_t.c","_w.c"]:
        NEEDED.append(f"botfiles/bots/{bot}{suffix}")

# ---------------------------------------------------------------------------

def build(gamedata_dir, output_dir):
    baseoa_dir = os.path.join(gamedata_dir, "baseoa")
    os.makedirs(output_dir, exist_ok=True)

    # Find all pk3 files in alphabetical order (later = higher priority)
    paks = sorted([
        os.path.join(baseoa_dir, f)
        for f in os.listdir(baseoa_dir)
        if f.lower().endswith(".pk3")
    ])
    if not paks:
        print(f"ERROR: No .pk3 files found in {baseoa_dir}", file=sys.stderr)
        sys.exit(1)

    print(f"Found {len(paks)} pak files.")

    # Build virtual filesystem: filename -> (pak_path, data)
    vfs = {}
    for pak_path in paks:
        try:
            with zipfile.ZipFile(pak_path, "r") as z:
                for name in z.namelist():
                    if not name.endswith("/"):
                        vfs[name] = (pak_path, z.read(name))
        except Exception as e:
            print(f"  WARNING: could not read {pak_path}: {e}", file=sys.stderr)

    # Extract needed files
    extracted = {}
    for need in NEEDED:
        if need in vfs:
            src, data = vfs[need]
            extracted[need] = data
            print(f"  OK  {need:50s}  {len(data):8d} bytes  <- {os.path.basename(src)}")
        else:
            print(f"  --  {need:50s}  (not found, skipping)")

    total = sum(len(d) for d in extracted.values())
    print(f"\n{len(extracted)} files extracted, {total/1024:.1f} KB uncompressed")

    # Repack into minipak.pk3
    minipak_path = os.path.join(output_dir, "minipak.pk3")
    with zipfile.ZipFile(minipak_path, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for name, data in sorted(extracted.items()):
            z.writestr(name, data)
    minipak_size = os.path.getsize(minipak_path)
    print(f"minipak.pk3: {minipak_size/1024:.1f} KB")

    # Load the autoexec.cfg from bench_config
    script_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    autoexec_src = os.path.join(script_dir, "bench_config", "baseoa", "autoexec.cfg")
    with open(autoexec_src, "rb") as f:
        autoexec_data = f.read()

    # Build CPIO (newc format) containing:
    #   baseoa/minipak.pk3
    #   baseoa/autoexec.cfg
    #   baseoa/q3config_server.cfg  (empty -- engine tries to read this)
    cpio_files = {
        "baseoa/minipak.pk3":           open(minipak_path, "rb").read(),
        "baseoa/autoexec.cfg":           autoexec_data,
        "baseoa/q3config_server.cfg":    b"",
    }

    cpio_path = os.path.join(output_dir, "gamedata.cpio")
    with open(cpio_path, "wb") as cpio:
        ino = 1
        for name, data in cpio_files.items():
            write_cpio_entry(cpio, ino, name, data)
            ino += 1
        # Trailer
        write_cpio_entry(cpio, 0, "TRAILER!!!", b"")

    cpio_size = os.path.getsize(cpio_path)
    print(f"gamedata.cpio: {cpio_size/1024:.1f} KB  ->  {cpio_path}")


def write_cpio_entry(f, ino, name, data):
    """Write one newc CPIO entry."""
    name_bytes = name.encode() + b"\x00"
    namesize = len(name_bytes)
    filesize = len(data)
    mode = 0o100644 if filesize > 0 else 0  # regular file

    header = (
        b"070701"
        + f"{ino:08x}".encode()
        + f"{mode:08x}".encode()
        + b"00000000"
        + b"00000000"
        + b"00000001"
        + b"00000000"
        + f"{filesize:08x}".encode()
        + b"00000001"
        + b"00000000"
        + b"00000000"
        + b"00000000"
        + f"{namesize:08x}".encode()
        + b"00000000"
    )
    assert len(header) == 110

    f.write(header)
    f.write(name_bytes)
    # Pad name to 4-byte boundary (header + name together)
    pad_name = (4 - (110 + namesize) % 4) % 4
    f.write(b"\x00" * pad_name)
    f.write(data)
    # Pad data to 4-byte boundary
    pad_data = (4 - filesize % 4) % 4 if filesize else 0
    f.write(b"\x00" * pad_data)


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <gamedata_dir> <output_dir>")
        sys.exit(1)
    build(sys.argv[1], sys.argv[2])
