# 
# Copyright (C) 2002 - 2004 Jeff Dike (jdike@addtoit.com)
# Licensed under the GPL
#

obj-y := exec_kern.o mem.o mem_user.o mmu.o process.o process_kern.o \
	syscall_kern.o syscall_user.o time.o tlb.o trap_user.o uaccess.o \

USER_OBJS = $(filter %_user.o,$(obj-y)) process.o time.o
USER_OBJS := $(foreach file,$(USER_OBJS),$(obj)/$(file))

$(USER_OBJS) : %.o: %.c
	$(CC) $(CFLAGS_$(notdir $@)) $(USER_CFLAGS) -c -o $@ $<

subdir- := util
