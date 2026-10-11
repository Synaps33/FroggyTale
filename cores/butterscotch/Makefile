# Butterscotch -> GB300V2 / SF2000 core, built for sf2000_multicore.
#
# Produces butterscotch_libretro_sf2000.a, which sf2000_multicore's Makefile links
# into core_87000000 -> system/Deimos/cores/butterscotch.sf2k.
#
# Called from the multicore side as:
#   make -C sf2000_multicore CORE=cores/butterscotch CONSOLE=butterscotch FROGGY_TYPE=GB300V2
#
# Toolchain notes (from sf2000_multicore/Makefile and xash3d-gb300/build_sf2000.py):
#   - -march MUST stay plain mips32.  The JZ4775 has no MIPS32r2 bit-manipulation
#     unit; seb/seh/ins/ext/wsbh/rotr raise ExcCode 10 and kill the core.
#     -mtune=mips32r2 is only a scheduling hint and is safe.
#   - no FPU, so -msoft-float.  Everything floating point goes through libgcc's
#     soft-float helpers, which is the single biggest performance risk on this port.
#   - -mno-abicalls -fno-pic -G0 so calls to firmware functions resolve to absolute
#     addresses from the linker script with no GOT.

TARGET_NAME := butterscotch
# BUTTERSCOTCH_SRC is this checkout's root.  MULTICORE_SRC is the *separate*
# sf2000_multicore tree (libretro frontend + firmware glue) that links this archive
# into the .sf2k core; it is not vendored here, so point at your checkout:
#   make MULTICORE_SRC=/path/to/sf2000_multicore
REPO_ROOT     := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
BUTTERSCOTCH_SRC ?= $(REPO_ROOT)
MULTICORE_SRC ?= $(abspath $(REPO_ROOT)/../sf2000_multicore)
LIBRETRO_COMMON ?= $(MULTICORE_SRC)/libs/libretro-common/include

ifeq ($(wildcard $(LIBRETRO_COMMON)/libretro.h),)
$(error libretro.h not found under MULTICORE_SRC='$(MULTICORE_SRC)'.
        Pass MULTICORE_SRC=/path/to/sf2000_multicore (the tree holding
        libs/libretro-common/include and include/deimos.h))
endif

MIPS    ?= /opt/frog-toolchain/mipsel-mti-elf_stable/bin/mipsel-mti-elf-
CC      := $(MIPS)gcc
# frog-toolchain ships only gcc/g++/ld/objcopy, so ar/ranlib come from the sibling
# binutils tree (classicube_sf2000/Makefile documents the same workaround).  Note the
# binutils there are plain `mips-mti-elf-`, not `mipsel-`, and that is fine: they build
# archives of objects, they do not emit code.
BINUTILS ?= /opt/mips32-mti-elf/2019.09-03-2/bin/mips-mti-elf-
AR      ?= $(BINUTILS)ar
RANLIB  ?= $(BINUTILS)ranlib

BS      := $(BUTTERSCOTCH_SRC)
TARGET  := $(TARGET_NAME)_libretro_sf2000.a
OBJDIR  := build/obj

# renderer=sw   -> software RGB565 renderer (src/sw_renderer.c)
# renderer=noop -> no-op renderer, for isolating VM/IO problems
RENDERER ?= sw

CFLAGS := -EL -march=mips32 -mtune=mips32r2 -msoft-float
CFLAGS += -G0 -mno-abicalls -fno-pic
CFLAGS += -ffunction-sections -fdata-sections
CFLAGS += -fno-strict-aliasing

# -Os everywhere by default: measured on this class of core, -O2 grows the image by
# ~50 kB of text and buys nothing that showed up on real hardware (openbor_sf2000
# README).  Override with OPT=-O2 when profiling.
OPT ?= -Os
CFLAGS += $(OPT) -fomit-frame-pointer

CFLAGS += -I$(BS)/src -I$(BS)/src/image -I$(BS)/src/video -I$(BS)/src/audio -I$(BS)/src/debug_font -I$(BS)/src/sf2000 -I$(BS)/src/sw_renderer
CFLAGS += -I$(BS)/vendor/stb/ds -I$(BS)/vendor/stb/image -I$(BS)/vendor/stb/vorbis
CFLAGS += -I$(BS)/vendor/bzip2 -I$(BS)/vendor/md5 -I$(BS)/vendor/miniz -I$(BS)/vendor/sha1 -I$(BS)/vendor/base64
CFLAGS += -I$(LIBRETRO_COMMON)
# This newlib's <dirent.h> is a stub that #error's out, and sf2000_multicore ships a
# working one on top of the firmware's fs_opendir/fs_readdir/fs_closedir.  It has to be
# on the include path *before* the sysroot so overlay_file_system.c picks it up.
CFLAGS += -I$(MULTICORE_SRC)/include

# Single-WAD build.  Only WAD 16 is enabled, which lets data_win.c collapse and drop
# all the other version checks - smaller image and a faster interpreter hot path.
# (CMakeLists.txt:110-113 makes the same argument for the PS2 builds.)
DEFINES := -DDATA_WIN_PATH=\"data.win\" \
           -DPLATFORM_SF2000 \
           -DENABLE_WAD16 -DDISABLE_WAD14 -DDISABLE_WAD17 \
           -DUSE_SDL2=0 \
           -DNO_THREADS

# Profiler / tracing / stub logging all off: they are pure overhead on a device this
# slow and each one adds branches to the VM inner loop.
DEFINES += -DDISABLE_VM_GML_PROFILER -DDISABLE_VM_OPCODE_PROFILER \
           -DDISABLE_VM_STUB_LOGS -DDISABLE_VM_TRACING -DDISABLE_SPATIAL_GRID_LOGS \
           -DDISABLE_VM_EXCEPTIONS_LOGS

# Audio: no OpenAL, no miniaudio.  src/audio/sf2000 provides the mixer instead.
# Video: no ffmpeg.
DEFINES += -DBUTTERSCOTCH_VIDEO_NULL=1

ifeq ($(RENDERER), sw)
	DEFINES += -DENABLE_SW_RENDERER
	SW_SRCS := $(wildcard $(BS)/src/sw_renderer/*.c)
else
	DEFINES += -DENABLE_NOOP_RENDERER
	SW_SRCS :=
endif

# Physics is off: Undertale does not use Box2D.  Note this is BOTH files - physics.c is
# the Physics_* wrapper the runner calls into and physics_disabled.c is the no-op
# PhysicsEngine_* backend behind it (CMakeLists.txt:160-166 pairs them the same way).
PHYS_SRCS := $(BS)/src/physics/physics.c \
             $(BS)/src/physics/disabled/physics_disabled.c

# Video: Undertale never calls video_open, so link the null backend.
VIDEO_SRCS := $(BS)/src/video/null_video.c

VENDOR_SRCS := $(wildcard $(BS)/vendor/bzip2/*.c) \
               $(wildcard $(BS)/vendor/miniz/*.c) \
               $(BS)/vendor/md5/md5.c \
               $(BS)/vendor/sha1/sha1.c \
               $(BS)/vendor/base64/base64.c \
               $(BS)/vendor/stb/vorbis/stb_vorbis.c

# src/*.c carries loop.c, vm*.c, data_win.c, runner.c, the noop_* reference
# implementations and log.c/gettime.c - all needed even in a GL-free build.
CORE_SRCS := $(filter-out $(BS)/src/embedded/%.c,$(wildcard $(BS)/src/*.c)) \
             $(wildcard $(BS)/src/image/*.c) \
             $(wildcard $(BS)/src/debug_font/*.c) \
             $(wildcard $(BS)/src/sf2000/*.c) \
             $(SW_SRCS) \
             $(PHYS_SRCS) \
             $(VIDEO_SRCS) \
             $(VENDOR_SRCS)

# Mirror the source tree under $(OBJDIR) instead of flattening to basenames: that
# keeps identically named files in different directories apart (stb_impl.c exists in
# cli/, embedded/, vita/, ps3/ and switch/) while still matching the pattern rule.
REL_SRCS := $(patsubst $(BS)/%,%,$(CORE_SRCS))
OBJS     := $(addprefix $(OBJDIR)/,$(REL_SRCS:.c=.o))

all: $(TARGET)

$(TARGET): $(OBJS)
	@rm -f $@
	$(AR) rcs $@ $(OBJS)
	$(RANLIB) $@
	@echo "built $@ ($$(stat -c %s $@) bytes, $(words $(OBJS)) objects)"

# Every object depends on a stamp holding the exact flag set.  Without this, flipping
# RENDERER (or any -D) leaves stale .o files in place, because the pattern rule only
# looks at the .c timestamp.  That bit us once: loop.o was built at 13:39 before
# RENDERER=sw was ever passed, so the "#ifdef ENABLE_SW_RENDERER" branch in loop.c was
# compiled out.  The core then linked fine and booted, but loop() rejected the request
# with "The software renderer is not available in this build!" and nothing was ever
# drawn - including SwAtlas_open(), so gc-sections dropped the whole renderer.
# The stamp makes a flag change rebuild everything that depends on it.
$(OBJDIR)/.flags.stamp: FORCE
	@mkdir -p $(dir $@)
	@printf '%s\n%s\n%s\n' '$(CC)' '$(CFLAGS)' '$(DEFINES)' > $@.tmp
	@if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv $@.tmp $@; \
	  echo "flags changed - rebuilding $(words $(OBJS)) objects"; fi

FORCE:

$(OBJDIR)/%.o: $(BS)/%.c $(OBJDIR)/.flags.stamp
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(DEFINES) -c -o $@ $<

clean:
	rm -rf build $(TARGET)

.PHONY: all clean FORCE
