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

ifeq ($(O),)
OUTPUT_DIR := $(CURDIR)/build
else
OUTPUT_DIR := $(O)/build
endif # O
OBJ_DIR := $(OUTPUT_DIR)/obj
INCLUDE_DIR := $(OUTPUT_DIR)/include
BPFOBJ_DIR := $(OBJ_DIR)/libbpf
SCXOBJ_DIR := $(OBJ_DIR)/sched_ext
BINDIR := $(OUTPUT_DIR)/bin
BPFOBJ := $(BPFOBJ_DIR)/libbpf.a
ifneq ($(CROSS_COMPILE),)
HOST_BUILD_DIR		:= $(OBJ_DIR)/host
HOST_OUTPUT_DIR	:= host-tools
HOST_INCLUDE_DIR	:= $(HOST_OUTPUT_DIR)/include
else
HOST_BUILD_DIR		:= $(OBJ_DIR)
HOST_OUTPUT_DIR	:= $(OUTPUT_DIR)
HOST_INCLUDE_DIR	:= $(INCLUDE_DIR)
endif
HOST_BPFOBJ := $(HOST_BUILD_DIR)/libbpf/libbpf.a
RESOLVE_BTFIDS := $(HOST_BUILD_DIR)/resolve_btfids/resolve_btfids
DEFAULT_BPFTOOL := $(HOST_OUTPUT_DIR)/sbin/bpftool

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

CARGOFLAGS := --release --target-dir $(OUTPUT_DIR)

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

all: scx_simple scx_qmap scx_central scx_pair scx_flatcg scx_userland scx_rusty

# sort removes libbpf duplicates when not cross-building
MAKE_DIRS := $(sort $(OBJ_DIR)/libbpf $(HOST_BUILD_DIR)/libbpf		\
	       $(HOST_BUILD_DIR)/bpftool $(HOST_BUILD_DIR)/resolve_btfids	\
	       $(INCLUDE_DIR) $(SCXOBJ_DIR) $(BINDIR))

$(MAKE_DIRS):
	$(call msg,MKDIR,,$@)
	$(Q)mkdir -p $@

$(BPFOBJ): $(wildcard $(BPFDIR)/*.[ch] $(BPFDIR)/Makefile)			\
	   $(APIDIR)/linux/bpf.h						\
	   | $(OBJ_DIR)/libbpf
	$(Q)$(MAKE) $(submake_extras) -C $(BPFDIR) OUTPUT=$(OBJ_DIR)/libbpf/	\
		    EXTRA_CFLAGS='-g -O0 -fPIC'					\
		    DESTDIR=$(OUTPUT_DIR) prefix= all install_headers

$(DEFAULT_BPFTOOL): $(wildcard $(BPFTOOLDIR)/*.[ch] $(BPFTOOLDIR)/Makefile)	\
		    $(HOST_BPFOBJ) | $(HOST_BUILD_DIR)/bpftool
	$(Q)$(MAKE) $(submake_extras)  -C $(BPFTOOLDIR)				\
		    ARCH= CROSS_COMPILE= CC=$(HOSTCC) LD=$(HOSTLD)		\
		    EXTRA_CFLAGS='-g -O0'					\
		    OUTPUT=$(HOST_BUILD_DIR)/bpftool/				\
		    LIBBPF_OUTPUT=$(HOST_BUILD_DIR)/libbpf/			\
		    LIBBPF_DESTDIR=$(HOST_OUTPUT_DIR)/				\
		    prefix= DESTDIR=$(HOST_OUTPUT_DIR)/ install-bin

$(INCLUDE_DIR)/vmlinux.h: $(VMLINUX_BTF) $(BPFTOOL) | $(INCLUDE_DIR)
ifeq ($(VMLINUX_H),)
	$(call msg,GEN,,$@)
	$(Q)$(BPFTOOL) btf dump file $(VMLINUX_BTF) format c > $@
else
	$(call msg,CP,,$@)
	$(Q)cp "$(VMLINUX_H)" $@
endif

%.bpf.o: %.bpf.c $(INCLUDE_DIR)/vmlinux.h scx_common.bpf.h user_exit_info.h	\
	| $(BPFOBJ) $(SCXOBJ_DIR)
	$(call msg,CLNG-BPF,,$@)
	$(Q)$(CLANG) $(BPF_CFLAGS) -target bpf -c $< -o $(SCXOBJ_DIR)/$@

%.skel.h: %.bpf.o $(BPFTOOL)
	$(call msg,GEN-SKEL,,$@)
	$(Q)$(BPFTOOL) gen object $(SCXOBJ_DIR)/$(<:.o=.linked1.o) $(SCXOBJ_DIR)/$<
	$(Q)$(BPFTOOL) gen object $(SCXOBJ_DIR)/$(<:.o=.linked2.o) $(SCXOBJ_DIR)/$(<:.o=.linked1.o)
	$(Q)$(BPFTOOL) gen object $(SCXOBJ_DIR)/$(<:.o=.linked3.o) $(SCXOBJ_DIR)/$(<:.o=.linked2.o)
	$(Q)diff $(SCXOBJ_DIR)/$(<:.o=.linked2.o) $(SCXOBJ_DIR)/$(<:.o=.linked3.o)
	$(Q)$(BPFTOOL) gen skeleton $(SCXOBJ_DIR)/$(<:.o=.linked3.o) name $(<:.bpf.o=) > $(INCLUDE_DIR)/$@
	$(Q)$(BPFTOOL) gen subskeleton $(SCXOBJ_DIR)/$(<:.o=.linked3.o) name $(<:.bpf.o=) > $(INCLUDE_DIR)/$(@:.skel.h=.subskel.h)

define ccsched
	$(CC) $(CFLAGS) -c $(1) -o $(SCXOBJ_DIR)/$(2).o
	$(CC) -o $(BINDIR)/$(2) $(SCXOBJ_DIR)/$(2).o $(HOST_BPFOBJ) $(LDFLAGS)
endef

SCX_COMMON_DEPS := user_exit_info.h scx_user_common.h | $(BINDIR)
scx_simple: scx_simple.c scx_simple.skel.h $(SCX_COMMON_DEPS)
	$(call ccsched,$<,$@)

scx_qmap: scx_qmap.c scx_qmap.skel.h $(SCX_COMMON_DEPS)
	$(call ccsched,$<,$@)

scx_central: scx_central.c scx_central.skel.h $(SCX_COMMON_DEPS)
	$(call ccsched,$<,$@)

scx_pair: scx_pair.c scx_pair.skel.h $(SCX_COMMON_DEPS)
	$(call ccsched,$<,$@)

scx_flatcg: scx_flatcg.c scx_flatcg.skel.h $(SCX_COMMON_DEPS)
	$(call ccsched,$<,$@)

scx_userland: scx_userland.c scx_userland.skel.h scx_userland.h $(SCX_COMMON_DEPS)
	$(call ccsched,$<,$@)

scx_rusty_deps: $(SCX_COMMON_DEPS)
	cargo fetch --manifest-path=scx_rusty/Cargo.toml
scx_rusty: export RUSTFLAGS = -C link-args=-lzstd -C link-args=-lz -C link-args=-lelf -L $(BPFOBJ_DIR)
scx_rusty: export SCX_RUSTY_CLANG = $(CLANG)
scx_rusty: export SCX_RUSTY_BPF_CFLAGS = $(BPF_CFLAGS)
scx_rusty: $(INCLUDE_DIR)/vmlinux.h scx_rusty_deps
	cargo build --manifest-path=$@/Cargo.toml --offline $(CARGOFLAGS)
	$(Q)cp $(OUTPUT_DIR)/release/$@ $(BINDIR)/$@

install: all
	$(Q)mkdir -p $(DESTDIR)/usr/bin/
	$(Q)cp $(BINDIR)/* $(DESTDIR)/usr/bin/

clean:
	cargo clean --manifest-path=scx_rusty/Cargo.toml
	rm -rf $(OUTPUT_DIR) $(HOST_OUTPUT_DIR)
	rm -f *.o *.bpf.o *.skel.h *.subskel.h
	rm -f scx_simple scx_qmap scx_central scx_pair scx_flatcg scx_userland

help:
	@echo  'Building targets:'
	@echo  '  all		  - Compile all schedulers'
	@echo  ''
	@echo  'Alternatively, you may compile individual schedulers:'
	@echo  '  scx_simple'
	@echo  '  scx_qmap'
	@echo  '  scx_central'
	@echo  '  scx_pair'
	@echo  '  scx_flatcg'
	@echo  '  scx_userland'
	@echo  '  scx_rusty'
	@echo  ''
	@echo  'For any scheduler build target, you may specify an alternative'
	@echo  'build output path with the O= environment variable. For example:'
	@echo  ''
	@echo  '   O=/tmp/sched_ext make all'
	@echo  ''
	@echo  'will compile all schedulers, and emit the build artifacts to'
	@echo  '/tmp/sched_ext/build.'
	@echo  ''
	@echo  ''
	@echo  'Installing targets:'
	@echo  '  install	  - Compile and install all schedulers to /usr/bin.'
	@echo  '		    You may specify the DESTDIR= environment variable'
	@echo  '		    to indicate a prefix for /usr/bin. For example:'
	@echo  ''
	@echo  '   DESTDIR=/tmp/sched_ext make install'
	@echo  ''
	@echo  '		    will build the schedulers in CWD/build, and'
	@echo  '		    install the schedulers to /tmp/sched_ext/usr/bin.'
	@echo  ''
	@echo  ''
	@echo  'Cleaning targets:'
	@echo  '  clean		  - Remove all generated files, including intermediate'
	@echo  '                    rust files for rust schedulers.'

.PHONY: all scx_rusty clean help

# delete failed targets
.DELETE_ON_ERROR:

# keep intermediate (.skel.h, .bpf.o, etc) targets
.SECONDARY:
