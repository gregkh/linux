#
# Copyright 2003 PathScale, Inc.
#
# Licensed under the GPL
#

lib-y = bitops.o bugs.o csum-partial.o delay.o fault.o mem.o memcpy.o \
	ptrace.o ptrace_user.o semaphore.o sigcontext.o signal.o \
	syscalls.o sysrq.o thunk.o

USER_OBJS := ptrace_user.o sigcontext.o
USER_OBJS := $(foreach file,$(USER_OBJS),$(obj)/$(file))

SYMLINKS = bitops.c csum-copy.S csum-partial.c csum-wrappers.c memcpy.S \
	semaphore.c thunk.S
SYMLINKS := $(foreach f,$(SYMLINKS),$(src)/$f)

clean-files := $(SYMLINKS)

bitops.c-dir = lib
csum-copy.S-dir = lib
csum-partial.c-dir = lib
csum-wrappers.c-dir = lib
memcpy.S-dir = lib
semaphore.c-dir = kernel
thunk.S-dir = lib

define make_link
       -rm -f $1
       ln -sf $(TOPDIR)/arch/x86_64/$($(notdir $1)-dir)/$(notdir $1) $1
endef

$(SYMLINKS):
	$(call make_link,$@)

$(USER_OBJS) : %.o: %.c
	$(CC) $(CFLAGS_$(notdir $@)) $(USER_CFLAGS) -c -o $@ $<

CFLAGS_csum-partial.o := -Dcsum_partial=arch_csum_partial
