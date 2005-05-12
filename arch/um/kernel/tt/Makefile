# 
# Copyright (C) 2002 - 2003 Jeff Dike (jdike@addtoit.com)
# Licensed under the GPL
#

extra-y := unmap_fin.o
clean-files := unmap_tmp.o

obj-y = exec_kern.o exec_user.o gdb.o ksyms.o mem.o mem_user.o process_kern.o \
	syscall_kern.o syscall_user.o time.o tlb.o tracer.o trap_user.o \
	uaccess.o uaccess_user.o

obj-$(CONFIG_PT_PROXY) += gdb_kern.o ptproxy/

USER_OBJS := $(filter %_user.o,$(obj-y)) gdb.o time.o tracer.o
USER_OBJS := $(foreach file,$(USER_OBJS),$(obj)/$(file))

UNMAP_CFLAGS := $(patsubst -pg -DPROFILING,,$(USER_CFLAGS))
UNMAP_CFLAGS := $(patsubst -fprofile-arcs -ftest-coverage,,$(UNMAP_CFLAGS))

$(USER_OBJS) : %.o: %.c
	$(CC) $(CFLAGS_$(notdir $@)) $(USER_CFLAGS) -c -o $@ $<

$(obj)/unmap.o: $(src)/unmap.c
	$(CC) $(UNMAP_CFLAGS) -c -o $@ $<

LIBC_DIR ?= /usr/lib

$(obj)/unmap_fin.o : $(obj)/unmap.o
	ld -r -o $(obj)/unmap_tmp.o  $< -lc -L$(LIBC_DIR)
	objcopy $(obj)/unmap_tmp.o $@ -G switcheroo

