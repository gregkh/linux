# SPDX-License-Identifier: GPL-2.0
# Copyright (c) 2022 Meta Platforms, Inc. and affiliates.
include ../build/Build.include
include ../scripts/Makefile.arch
include ../scripts/Makefile.include

ifneq ($(LLVM),)
ifneq ($(filter %/,$(LLVM)),)
LLVM_PREFIX := $(LLVM)
else ifneq ($(filter -%,$(LLVM)),)
LLVM_SUFFIX := $(LLVM)
endif

CLANG_TARGET_FLAGS_arm          := arm-linux-gnueabi
CLANG_TARGET_FLAGS_arm64        := aarch64-linux-gnu
CLANG_TARGET_FLAGS_hexagon      := hexagon-linux-musl
CLANG_TARGET_FLAGS_m68k         := m68k-linux-gnu
CLANG_TARGET_FLAGS_mips         := mipsel-linux-gnu
CLANG_TARGET_FLAGS_powerpc      := powerpc64le-linux-gnu
CLANG_TARGET_FLAGS_riscv        := riscv64-linux-gnu
CLANG_TARGET_FLAGS_s390         := s390x-linux-gnu
CLANG_TARGET_FLAGS_x86          := x86_64-linux-gnu
CLANG_TARGET_FLAGS              := $(CLANG_TARGET_FLAGS_$(ARCH))

ifeq ($(CROSS_COMPILE),)
ifeq ($(CLANG_TARGET_FLAGS),)
$(error Specify CROSS_COMPILE or add '--target=' option to lib.mk
else
CLANG_FLAGS     += --target=$(CLANG_TARGET_FLAGS)
endif # CLANG_TARGET_FLAGS
else
CLANG_FLAGS     += --target=$(notdir $(CROSS_COMPILE:%-=%))
endif # CROSS_COMPILE

CC := $(LLVM_PREFIX)clang$(LLVM_SUFFIX) $(CLANG_FLAGS) -fintegrated-as
else
CC := $(CROSS_COMPILE)gcc
endif # LLVM

CURDIR := $(abspath .)
TOOLSDIR := $(abspath ..)
LIBDIR := $(TOOLSDIR)/lib
BPFDIR := $(LIBDIR)/bpf
TOOLSINCDIR := $(TOOLSDIR)/include
BPFTOOLDIR := $(TOOLSDIR)/bpf/bpftool
APIDIR := $(TOOLSINCDIR)/uapi
GENDIR := $(abspath ../../include/generated)
GENHDR := $(GENDIR)/autoconf.h

SCRATCH_DIR := $(CURDIR)/tools
BUILD_DIR := $(SCRATCH_DIR)/build
INCLUDE_DIR := $(SCRATCH_DIR)/include
BPFOBJ_DIR := $(BUILD_DIR)/libbpf
BPFOBJ := $(BPFOBJ_DIR)/libbpf.a
ifneq ($(CROSS_COMPILE),)
HOST_BUILD_DIR		:= $(BUILD_DIR)/host
HOST_SCRATCH_DIR	:= host-tools
HOST_INCLUDE_DIR	:= $(HOST_SCRATCH_DIR)/include
else
HOST_BUILD_DIR		:= $(BUILD_DIR)
HOST_SCRATCH_DIR	:= $(SCRATCH_DIR)
HOST_INCLUDE_DIR	:= $(INCLUDE_DIR)
endif
HOST_BPFOBJ := $(HOST_BUILD_DIR)/libbpf/libbpf.a
RESOLVE_BTFIDS := $(HOST_BUILD_DIR)/resolve_btfids/resolve_btfids
DEFAULT_BPFTOOL := $(HOST_SCRATCH_DIR)/sbin/bpftool

VMLINUX_BTF_PATHS ?= $(if $(O),$(O)/vmlinux)					\
		     $(if $(KBUILD_OUTPUT),$(KBUILD_OUTPUT)/vmlinux)		\
		     ../../vmlinux						\
		     /sys/kernel/btf/vmlinux					\
		     /boot/vmlinux-$(shell uname -r)
VMLINUX_BTF ?= $(abspath $(firstword $(wildcard $(VMLINUX_BTF_PATHS))))
ifeq ($(VMLINUX_BTF),)
$(error Cannot find a vmlinux for VMLINUX_BTF at any of "$(VMLINUX_BTF_PATHS)")
endif

BPFTOOL ?= $(DEFAULT_BPFTOOL)

ifneq ($(wildcard $(GENHDR)),)
  GENFLAGS := -DHAVE_GENHDR
endif

CFLAGS += -g -O2 -rdynamic -pthread -Wall -Werror $(GENFLAGS)			\
	  -I$(INCLUDE_DIR) -I$(GENDIR) -I$(LIBDIR)				\
	  -I$(TOOLSINCDIR) -I$(APIDIR)

CARGOFLAGS := --release

# Silence some warnings when compiled with clang
ifneq ($(LLVM),)
CFLAGS += -Wno-unused-command-line-argument
endif

LDFLAGS = -lelf -lz -lpthread

IS_LITTLE_ENDIAN = $(shell $(CC) -dM -E - </dev/null |				\
			grep 'define __BYTE_ORDER__ __ORDER_LITTLE_ENDIAN__')

# Get Clang's default includes on this system, as opposed to those seen by
# '-target bpf'. This fixes "missing" files on some architectures/distros,
# such as asm/byteorder.h, asm/socket.h, asm/sockios.h, sys/cdefs.h etc.
#
# Use '-idirafter': Don't interfere with include mechanics except where the
# build would have failed anyways.
define get_sys_includes
$(shell $(1) -v -E - </dev/null 2>&1 \
	| sed -n '/<...> search starts here:/,/End of search list./{ s| \(/.*\)|-idirafter \1|p }') \
$(shell $(1) -dM -E - </dev/null | grep '__riscv_xlen ' | awk '{printf("-D__riscv_xlen=%d -D__BITS_PER_LONG=%d", $$3, $$3)}')
endef

BPF_CFLAGS = -g -D__TARGET_ARCH_$(SRCARCH)					\
	     $(if $(IS_LITTLE_ENDIAN),-mlittle-endian,-mbig-endian)		\
	     -I$(INCLUDE_DIR) -I$(CURDIR) -I$(APIDIR)				\
	     -I../../include							\
	     $(call get_sys_includes,$(CLANG))					\
	     -Wall -Wno-compare-distinct-pointer-types				\
	     -O2 -mcpu=v3

all: scx_simple scx_qmap scx_central scx_pair scx_flatcg scx_userland scx_atropos

# sort removes libbpf duplicates when not cross-building
MAKE_DIRS := $(sort $(BUILD_DIR)/libbpf $(HOST_BUILD_DIR)/libbpf		\
	       $(HOST_BUILD_DIR)/bpftool $(HOST_BUILD_DIR)/resolve_btfids	\
	       $(INCLUDE_DIR))

$(MAKE_DIRS):
	$(call msg,MKDIR,,$@)
	$(Q)mkdir -p $@

$(BPFOBJ): $(wildcard $(BPFDIR)/*.[ch] $(BPFDIR)/Makefile)			\
	   $(APIDIR)/linux/bpf.h						\
	   | $(BUILD_DIR)/libbpf
	$(Q)$(MAKE) $(submake_extras) -C $(BPFDIR) OUTPUT=$(BUILD_DIR)/libbpf/	\
		    EXTRA_CFLAGS='-g -O0 -fPIC'					\
		    DESTDIR=$(SCRATCH_DIR) prefix= all install_headers

$(DEFAULT_BPFTOOL): $(wildcard $(BPFTOOLDIR)/*.[ch] $(BPFTOOLDIR)/Makefile)	\
		    $(HOST_BPFOBJ) | $(HOST_BUILD_DIR)/bpftool
	$(Q)$(MAKE) $(submake_extras)  -C $(BPFTOOLDIR)				\
		    ARCH= CROSS_COMPILE= CC=$(HOSTCC) LD=$(HOSTLD)		\
		    EXTRA_CFLAGS='-g -O0'					\
		    OUTPUT=$(HOST_BUILD_DIR)/bpftool/				\
		    LIBBPF_OUTPUT=$(HOST_BUILD_DIR)/libbpf/			\
		    LIBBPF_DESTDIR=$(HOST_SCRATCH_DIR)/				\
		    prefix= DESTDIR=$(HOST_SCRATCH_DIR)/ install-bin

$(INCLUDE_DIR)/vmlinux.h: $(VMLINUX_BTF) $(BPFTOOL) | $(INCLUDE_DIR)
ifeq ($(VMLINUX_H),)
	$(call msg,GEN,,$@)
	$(Q)$(BPFTOOL) btf dump file $(VMLINUX_BTF) format c > $@
else
	$(call msg,CP,,$@)
	$(Q)cp "$(VMLINUX_H)" $@
endif

%.bpf.o: %.bpf.c $(INCLUDE_DIR)/vmlinux.h scx_common.bpf.h user_exit_info.h	\
	| $(BPFOBJ)
	$(call msg,CLNG-BPF,,$@)
	$(Q)$(CLANG) $(BPF_CFLAGS) -target bpf -c $< -o $@

%.skel.h: %.bpf.o $(BPFTOOL)
	$(call msg,GEN-SKEL,,$@)
	$(Q)$(BPFTOOL) gen object $(<:.o=.linked1.o) $<
	$(Q)$(BPFTOOL) gen object $(<:.o=.linked2.o) $(<:.o=.linked1.o)
	$(Q)$(BPFTOOL) gen object $(<:.o=.linked3.o) $(<:.o=.linked2.o)
	$(Q)diff $(<:.o=.linked2.o) $(<:.o=.linked3.o)
	$(Q)$(BPFTOOL) gen skeleton $(<:.o=.linked3.o) name $(<:.bpf.o=) > $@
	$(Q)$(BPFTOOL) gen subskeleton $(<:.o=.linked3.o) name $(<:.bpf.o=) > $(@:.skel.h=.subskel.h)

scx_simple: scx_simple.c scx_simple.skel.h user_exit_info.h
	$(CC) $(CFLAGS) -c $< -o $@.o
	$(CC) -o $@ $@.o $(HOST_BPFOBJ) $(LDFLAGS)

scx_qmap: scx_qmap.c scx_qmap.skel.h user_exit_info.h
	$(CC) $(CFLAGS) -c $< -o $@.o
	$(CC) -o $@ $@.o $(HOST_BPFOBJ) $(LDFLAGS)

scx_central: scx_central.c scx_central.skel.h user_exit_info.h
	$(CC) $(CFLAGS) -c $< -o $@.o
	$(CC) -o $@ $@.o $(HOST_BPFOBJ) $(LDFLAGS)

scx_pair: scx_pair.c scx_pair.skel.h user_exit_info.h
	$(CC) $(CFLAGS) -c $< -o $@.o
	$(CC) -o $@ $@.o $(HOST_BPFOBJ) $(LDFLAGS)

scx_flatcg: scx_flatcg.c scx_flatcg.skel.h user_exit_info.h
	$(CC) $(CFLAGS) -c $< -o $@.o
	$(CC) -o $@ $@.o $(HOST_BPFOBJ) $(LDFLAGS)

scx_userland: scx_userland.c scx_userland.skel.h scx_userland.h user_exit_info.h
	$(CC) $(CFLAGS) -c $< -o $@.o
	$(CC) -o $@ $@.o $(HOST_BPFOBJ) $(LDFLAGS)

scx_atropos: export RUSTFLAGS = -C link-args=-lzstd -C link-args=-lz -C link-args=-lelf -L $(BPFOBJ_DIR)
scx_atropos: export ATROPOS_CLANG = $(CLANG)
scx_atropos: export ATROPOS_BPF_CFLAGS = $(BPF_CFLAGS)
scx_atropos: $(INCLUDE_DIR)/vmlinux.h
	cargo build --manifest-path=scx_atropos/Cargo.toml $(CARGOFLAGS)

clean:
	cargo clean --manifest-path=scx_atropos/Cargo.toml
	rm -rf $(SCRATCH_DIR) $(HOST_SCRATCH_DIR)
	rm -f *.o *.bpf.o *.skel.h *.subskel.h
	rm -f scx_simple scx_qmap scx_central scx_pair scx_flatcg scx_userland

.PHONY: all scx_atropos clean

# delete failed targets
.DELETE_ON_ERROR:

# keep intermediate (.skel.h, .bpf.o, etc) targets
.SECONDARY:
