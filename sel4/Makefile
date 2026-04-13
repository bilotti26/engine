# =============================================================================
# sel4/Makefile  -  Build the seL4 Microkit OA benchmark image
#
# Required variables (set by run_sel4_bench.sh or on the command line):
#   MICROKIT_SDK   path to extracted microkit-sdk-X.Y.Z/
#   DATA_DIR       path to directory containing gamedata.cpio
#   BUILD_DIR      output directory (default: sel4/build)
#   BENCH_FRAMES   frames to run (default: 1000)
#
# Usage:
#   make -C sel4 MICROKIT_SDK=/tmp/microkit-sdk-1.4.1 DATA_DIR=sel4/data
# =============================================================================

MICROKIT_BOARD   ?= qemu_virt_aarch64
# Use debug config: seL4 debug kernel enables microkit_dbg_puts/seL4_DebugPutChar
# which routes serial output to QEMU's UART.  Release builds are silent.
MICROKIT_CONFIG  ?= debug
BENCH_FRAMES     ?= 1000
BUILD_DIR        ?= $(CURDIR)/build
REPO_ROOT        := $(abspath $(CURDIR)/..)

ifndef MICROKIT_SDK
$(error MICROKIT_SDK must be set to the path of the Microkit SDK)
endif
ifndef DATA_DIR
$(error DATA_DIR must be set to the directory containing gamedata.cpio)
endif

BOARD_DIR := $(MICROKIT_SDK)/board/$(MICROKIT_BOARD)/$(MICROKIT_CONFIG)
MICROKIT_TOOL := $(MICROKIT_SDK)/bin/microkit

# ---------------------------------------------------------------------------
# Toolchain
# ---------------------------------------------------------------------------
# Use aarch64-linux-gnu-gcc as a bare-metal cross-compiler.
# If aarch64-none-elf-gcc is available it's preferred.
ifneq ($(shell which aarch64-none-elf-gcc 2>/dev/null),)
  CC := aarch64-none-elf-gcc
  LD := aarch64-none-elf-ld
  AR := aarch64-none-elf-ar
  OBJCOPY := aarch64-none-elf-objcopy
else
  CC := aarch64-linux-gnu-gcc
  LD := aarch64-linux-gnu-ld
  AR := aarch64-linux-gnu-ar
  OBJCOPY := aarch64-linux-gnu-objcopy
endif

CPU := cortex-a53

# ---------------------------------------------------------------------------
# Compiler flags
# ---------------------------------------------------------------------------
CFLAGS := \
  -mcpu=$(CPU) \
  -nostdlib \
  -ffreestanding \
  -fno-stack-protector \
  -fno-builtin \
  -O2 \
  -g \
  -Wall \
  -U_FORTIFY_SOURCE \
  -D_FORTIFY_SOURCE=0 \
  -Wno-unused-function \
  -Wno-unused-variable \
  -Wno-implicit-function-declaration \
  -Wno-pointer-sign \
  -DDEDICATED \
  -DMEMTEST_BUILD \
  -DMEMTEST_SEL4 \
  -DBOTLIB \
  -DNO_VM_COMPILED \
  -DBENCH_FRAMES=$(BENCH_FRAMES) \
  -DUSE_ICON=0 \
  -DUSE_LOCAL_HEADERS \
  -DSTANDALONE \
  -I$(BOARD_DIR)/include \
  -I$(CURDIR)/include \
  -I$(REPO_ROOT)/code \
  -I$(REPO_ROOT)/code/qcommon \
  -I$(REPO_ROOT)/code/server \
  -I$(REPO_ROOT)/code/botlib \
  -I$(REPO_ROOT)/code/zlib

LDFLAGS := \
  -nostdlib \
  -Wl,--build-id=none \
  -L$(BOARD_DIR)/lib \
  -Tmicrokit.ld

LIBS := -lmicrokit

# ---------------------------------------------------------------------------
# Compiler flags for the minimal detector PDs (no engine headers / defines)
# ---------------------------------------------------------------------------
DET_CFLAGS := \
  -mcpu=$(CPU) \
  -nostdlib \
  -ffreestanding \
  -fno-stack-protector \
  -fno-builtin \
  -O2 \
  -g \
  -Wall \
  -Wno-unused-function \
  -I$(BOARD_DIR)/include \
  -I$(CURDIR)/include

# ---------------------------------------------------------------------------
# Source files
# ---------------------------------------------------------------------------
# seL4 platform sources
SEL4_SRCS := \
  $(CURDIR)/src/entry.c \
  $(CURDIR)/src/fs_cpio.c \
  $(CURDIR)/src/libc_mini.c \
  $(CURDIR)/src/libc_printf.c

# Engine system sources (seL4 platform backend replaces sys_unix.c)
SYS_SRCS := \
  $(REPO_ROOT)/code/sys/sys_bench.c \
  $(REPO_ROOT)/code/sys/sys_sel4.c \
  $(REPO_ROOT)/code/sys/con_log.c

# Null/stub sources
NULL_SRCS := \
  $(REPO_ROOT)/code/null/null_client.c \
  $(REPO_ROOT)/code/null/null_input.c \
  $(REPO_ROOT)/code/null/null_snddma.c \
  $(REPO_ROOT)/code/null/null_net_ip.c

# Engine core
QCOMMON_SRCS := \
  $(REPO_ROOT)/code/qcommon/cm_load.c \
  $(REPO_ROOT)/code/qcommon/cm_patch.c \
  $(REPO_ROOT)/code/qcommon/cm_polylib.c \
  $(REPO_ROOT)/code/qcommon/cm_test.c \
  $(REPO_ROOT)/code/qcommon/cm_trace.c \
  $(REPO_ROOT)/code/qcommon/cmd.c \
  $(REPO_ROOT)/code/qcommon/common.c \
  $(REPO_ROOT)/code/qcommon/cvar.c \
  $(REPO_ROOT)/code/qcommon/files.c \
  $(REPO_ROOT)/code/qcommon/huffman.c \
  $(REPO_ROOT)/code/qcommon/md4.c \
  $(REPO_ROOT)/code/qcommon/md5.c \
  $(REPO_ROOT)/code/qcommon/msg.c \
  $(REPO_ROOT)/code/qcommon/net_chan.c \
  $(REPO_ROOT)/code/qcommon/q_math.c \
  $(REPO_ROOT)/code/qcommon/q_shared.c \
  $(REPO_ROOT)/code/qcommon/ioapi.c \
  $(REPO_ROOT)/code/qcommon/unzip.c \
  $(REPO_ROOT)/code/qcommon/vm.c \
  $(REPO_ROOT)/code/qcommon/vm_interpreted.c \
  $(REPO_ROOT)/code/qcommon/vm_none.c \
  $(REPO_ROOT)/code/zlib/adler32.c \
  $(REPO_ROOT)/code/zlib/crc32.c \
  $(REPO_ROOT)/code/zlib/inffast.c \
  $(REPO_ROOT)/code/zlib/inflate.c \
  $(REPO_ROOT)/code/zlib/inftrees.c \
  $(REPO_ROOT)/code/zlib/zutil.c

SERVER_SRCS := \
  $(REPO_ROOT)/code/server/sv_bot.c \
  $(REPO_ROOT)/code/server/sv_ccmds.c \
  $(REPO_ROOT)/code/server/sv_client.c \
  $(REPO_ROOT)/code/server/sv_game.c \
  $(REPO_ROOT)/code/server/sv_init.c \
  $(REPO_ROOT)/code/server/sv_main.c \
  $(REPO_ROOT)/code/server/sv_net_chan.c \
  $(REPO_ROOT)/code/server/sv_snapshot.c \
  $(REPO_ROOT)/code/server/sv_world.c

BOTLIB_SRCS := \
  $(wildcard $(REPO_ROOT)/code/botlib/be_aas_*.c) \
  $(wildcard $(REPO_ROOT)/code/botlib/be_ai_*.c) \
  $(wildcard $(REPO_ROOT)/code/botlib/be_interface.c) \
  $(wildcard $(REPO_ROOT)/code/botlib/be_ea.c) \
  $(wildcard $(REPO_ROOT)/code/botlib/l_*.c)

ALL_SRCS := $(SEL4_SRCS) $(SYS_SRCS) $(NULL_SRCS) $(QCOMMON_SRCS) \
            $(SERVER_SRCS) $(BOTLIB_SRCS)

# ---------------------------------------------------------------------------
# Object files
# ---------------------------------------------------------------------------
_OBJ_FROM_SRC = $(patsubst $(REPO_ROOT)/%.c,$(BUILD_DIR)/engine/%.o,\
                 $(patsubst $(CURDIR)/%.c,$(BUILD_DIR)/sel4/%.o,$1))

OBJS := $(call _OBJ_FROM_SRC,$(ALL_SRCS)) \
        $(BUILD_DIR)/gamedata.o

# ---------------------------------------------------------------------------
# Targets
# ---------------------------------------------------------------------------
IMAGE_FILE   := $(BUILD_DIR)/loader.img
REPORT_FILE  := $(BUILD_DIR)/report.txt
ELF_FILE     := $(BUILD_DIR)/bench.elf
MONITOR_ELF  := $(BUILD_DIR)/monitor.elf
PHYSICS_ELF  := $(BUILD_DIR)/physics.elf

MONITOR_OBJ  := $(BUILD_DIR)/sel4/src/monitor.o
PHYSICS_OBJ  := $(BUILD_DIR)/sel4/src/physics.o

.PHONY: all clean

all: $(IMAGE_FILE)

# Embed game data CPIO as a binary object.
# Must cd to DATA_DIR so objcopy generates short symbol names:
#   _binary_gamedata_cpio_start / _end / _size
$(BUILD_DIR)/gamedata.o: $(DATA_DIR)/gamedata.cpio | $(BUILD_DIR)
	cd $(DATA_DIR) && $(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  gamedata.cpio $(BUILD_DIR)/gamedata.o

# Compile engine sources
$(BUILD_DIR)/engine/%.o: $(REPO_ROOT)/%.c | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) -c $(CFLAGS) -o $@ $<

# Compile sel4/ sources
$(BUILD_DIR)/sel4/%.o: $(CURDIR)/%.c | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) -c $(CFLAGS) -o $@ $<

# Link the main engine PD ELF
$(ELF_FILE): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

# Compile detector PD sources with minimal flags (no engine defines)
$(MONITOR_OBJ): $(CURDIR)/src/monitor.c | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) -c $(DET_CFLAGS) -o $@ $<

$(PHYSICS_OBJ): $(CURDIR)/src/physics.c | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) -c $(DET_CFLAGS) -o $@ $<

# Link detector PD ELFs (minimal: just libmicrokit)
$(MONITOR_ELF): $(MONITOR_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

$(PHYSICS_ELF): $(PHYSICS_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

# Package all three PD ELFs with the Microkit tool
$(IMAGE_FILE) $(REPORT_FILE): $(ELF_FILE) $(MONITOR_ELF) $(PHYSICS_ELF) $(CURDIR)/bench.system
	$(MICROKIT_TOOL) $(CURDIR)/bench.system \
	  --search-path $(BUILD_DIR) \
	  --board $(MICROKIT_BOARD) \
	  --config $(MICROKIT_CONFIG) \
	  -o $(IMAGE_FILE) \
	  -r $(REPORT_FILE)
	@echo ""
	@echo "seL4 image: $(IMAGE_FILE)"

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)/engine $(BUILD_DIR)/sel4 $(BUILD_DIR)/sel4/src

clean:
	rm -rf $(BUILD_DIR)
